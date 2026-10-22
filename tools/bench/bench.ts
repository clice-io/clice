/// E2E benchmark harness: drive language servers over LSP through fixed
/// scenarios on a real workspace and report client-observed latencies, the
/// memory of the server's processes and the size of its index.
///
/// The harness is server-agnostic at the LSP level, so the same scenarios
/// run against clice and clangd for A/B comparison: given both servers, it
/// alternates them run by run on the same files. For clice it additionally
/// parses the session's `[perf:*]` log lines — master and worker files
/// under <workspace>/.clice/logs — into a server-side breakdown (see
/// perf.ts). compare.ts turns the JSON of one or more runs into tables.
///
/// Usage:
///   node tools/bench/bench.ts --workspace <dir> [options]
///
/// Options:
///   --server clice|clangd    a server to drive, repeatable (default clice)
///   --clice <path>           clice executable (default
///                            build/RelWithDebInfo/bin/bin/clice)
///   --clangd <path>          clangd executable (default clangd from PATH)
///   --clangd-arg <arg>       an extra clangd argument, repeatable
///   --file <rel>             file to open/edit (default: first CDB entry)
///   --probe <kind>=<l>:<c>   a request position, repeatable. Kinds: symbol
///                            (hover, definition, references, call
///                            hierarchy; default: the file's first call-like
///                            identifier), member, unqualified and template
///                            (completion there), call (signature help).
///                            A request without its probe is skipped.
///   --query <text>           workspace symbol query (default: the
///                            identifier at the symbol probe)
///   --scenario <name>        cold_start | index | warm_start | edit_loop |
///                            warm_requests; repeatable (default: all, in
///                            this order)
///   --runs <N>               runs of every scenario per server (default 1)
///   --drop-caches            drop the OS file cache before every cold_start
///                            and index run (Linux and macOS, through sudo)
///   --edits <N>              edit_loop iterations (default 10)
///   --repeats <N>            warm request repetitions (default 50)
///   --json <path>            write the full results as JSON
///
/// The workspace must contain a compile_commands.json. Every run starts a
/// fresh server in its default configuration. cold_start and index wipe
/// the server's cache first (clice's .clice dir, clangd's .cache/clangd);
/// the other scenarios reuse what the earlier ones left, so warm_start
/// after index restarts on a full index.

import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { parseArgs } from "node:util";
import type * as proto from "vscode-languageserver-protocol";
import { CliceClient } from "../client/client.ts";
import { Workspace } from "../client/workspace.ts";
import { logFiles } from "../process_gate.ts";
import { withTimeout } from "../promise.ts";
import { REPO_ROOT } from "../repo.ts";
import { watchMemory, type WatchedMemory } from "./memory.ts";
import { computeStats, parsePerfLines, summarize } from "./perf.ts";
import {
    PROBES,
    SCENARIOS,
    SERVERS,
    type BenchResult,
    type ProbeKind,
    type RunResult,
    type ScenarioName,
    type ServerName,
} from "./results.ts";

/// Hang detection: a cold compile of a large project's largest file
/// (LLVM's SemaExpr.cpp) on a 3-core runner, with room to spare.
const COMPILE_BOUND = 600_000;
/// Hang detection: a full background index of LLVM on a 3-core runner.
const INDEX_BOUND = 6 * 3600_000;
/// How long the background index must stay idle to count as done: both
/// servers may end one round and begin another for files found meanwhile.
const INDEX_QUIET_MS = 15_000;

interface Options {
    servers: ServerName[];
    binaries: Record<ServerName, string>;
    clangdArgs: string[];
    workspace: string;
    /// Directory holding compile_commands.json; resolved in main().
    cdbDir: string;
    file: string;
    probes: Partial<Record<ProbeKind, proto.Position>>;
    query: string;
    scenarios: ScenarioName[];
    runs: number;
    dropCaches: boolean;
    edits: number;
    repeats: number;
    jsonPath: string | null;
}

function fail(message: string): never {
    console.error(`error: ${message}`);
    process.exit(1);
}

function parsePosition(text: string, what: string): proto.Position {
    const parts = text.split(":");
    const line = Number(parts[0]);
    const character = Number(parts[1]);
    if (
        parts.length !== 2 ||
        !Number.isInteger(line) ||
        line < 0 ||
        !Number.isInteger(character) ||
        character < 0
    ) {
        fail(`bad ${what} '${text}' (expected line:char)`);
    }
    return { line, character };
}

function count(text: string, what: string): number {
    const value = Number(text);
    if (!Number.isInteger(value) || value < 0) {
        fail(`${what} must be a non-negative integer`);
    }
    return value;
}

/// A path is anchored here, before clangd is started from the CDB
/// directory instead; a bare name stays a PATH lookup.
function anchored(binary: string): string {
    return path.basename(binary) === binary ? binary : path.resolve(binary);
}

function parseOptions(): Options {
    const { values } = parseArgs({
        options: {
            server: { type: "string", multiple: true },
            clice: { type: "string" },
            clangd: { type: "string", default: "clangd" },
            "clangd-arg": { type: "string", multiple: true },
            workspace: { type: "string" },
            file: { type: "string" },
            probe: { type: "string", multiple: true },
            query: { type: "string" },
            scenario: { type: "string", multiple: true },
            runs: { type: "string", default: "1" },
            "drop-caches": { type: "boolean", default: false },
            edits: { type: "string", default: "10" },
            repeats: { type: "string", default: "50" },
            json: { type: "string" },
            help: { type: "boolean", default: false },
        },
    });

    if (values.help || values.workspace === undefined) {
        console.log("Usage: node tools/bench/bench.ts --workspace <dir> [options]");
        console.log("See the header of tools/bench/bench.ts for the option list.");
        process.exit(values.help ? 0 : 1);
    }

    const servers = (values.server ?? ["clice"]).map((name) => {
        if (!(SERVERS as readonly string[]).includes(name)) {
            fail(`unknown server '${name}' (expected clice or clangd)`);
        }
        return name as ServerName;
    });
    const scenarios = (values.scenario ?? [...SCENARIOS]).map((name) => {
        if (!(SCENARIOS as readonly string[]).includes(name)) {
            fail(`unknown scenario '${name}'`);
        }
        return name as ScenarioName;
    });

    const probes: Partial<Record<ProbeKind, proto.Position>> = {};
    for (const probe of values.probe ?? []) {
        const [kind = "", at = ""] = probe.split("=");
        if (!(PROBES as readonly string[]).includes(kind)) {
            fail(`unknown probe '${kind}' (expected one of ${PROBES.join(", ")})`);
        }
        probes[kind as ProbeKind] = parsePosition(at, `--probe ${kind}`);
    }

    return {
        servers,
        binaries: {
            clice: anchored(
                values.clice ??
                    path.join(REPO_ROOT, "build", "RelWithDebInfo", "bin", "bin", "clice"),
            ),
            clangd: anchored(values.clangd),
        },
        clangdArgs: values["clangd-arg"] ?? [],
        workspace: path.resolve(values.workspace),
        cdbDir: "",
        file: values.file ?? "",
        probes,
        query: values.query ?? "",
        scenarios,
        runs: count(values.runs, "--runs"),
        dropCaches: values["drop-caches"],
        edits: count(values.edits, "--edits"),
        repeats: count(values.repeats, "--repeats"),
        jsonPath: values.json ?? null,
    };
}

/// Overwrite comment and string/char literal bytes with spaces, keeping
/// offsets and newlines intact, so position derivation only ever sees
/// code. Raw string literals are treated as ordinary strings — good
/// enough for a probe heuristic with a --probe override.
function blankNonCode(text: string): string {
    const out = text.split("");
    const blank = (index: number): void => {
        if (index < out.length && out[index] !== "\n") {
            out[index] = " ";
        }
    };
    let i = 0;
    while (i < text.length) {
        const two = text.slice(i, i + 2);
        if (two === "//") {
            while (i < text.length && text[i] !== "\n") {
                blank(i);
                i += 1;
            }
        } else if (two === "/*") {
            while (i < text.length && text.slice(i, i + 2) !== "*/") {
                blank(i);
                i += 1;
            }
            blank(i);
            blank(i + 1);
            i += 2;
        } else if (text[i] === '"' || text[i] === "'") {
            const quote = text[i];
            blank(i);
            i += 1;
            // An unterminated literal ends at the newline (blank keeps it).
            while (i < text.length && text[i] !== quote && text[i] !== "\n") {
                blank(i);
                if (text[i] === "\\") {
                    blank(i + 1);
                    i += 1;
                }
                i += 1;
            }
            blank(i);
            i += 1;
        } else {
            i += 1;
        }
    }
    return out.join("");
}

/// Derive a symbol-bearing probe position: the first call-like identifier
/// in code text — comments, string/char literals and preprocessor lines
/// do not count. The old 0:0 default usually landed on a license comment,
/// so the positional features took their empty-result fast paths and
/// measured nothing.
function derivePosition(file: string): proto.Position {
    const keywords = new Set([
        "if",
        "for",
        "while",
        "switch",
        "return",
        "sizeof",
        "catch",
        "static_assert",
        "decltype",
        "alignof",
        "noexcept",
    ]);
    const lines = blankNonCode(fs.readFileSync(file, "utf8")).split("\n");
    for (let line = 0; line < lines.length; line += 1) {
        const text = lines[line] ?? "";
        if (text.trimStart().startsWith("#")) {
            continue;
        }
        for (const match of text.matchAll(/\b([A-Za-z_]\w+)\s*\(/g)) {
            const name = match[1];
            if (name !== undefined && !keywords.has(name)) {
                return { line, character: match.index };
            }
        }
    }
    fail(`cannot derive a symbol-bearing position in ${file}; pass --probe symbol=<l>:<c>`);
}

function identifierAt(file: string, at: proto.Position): string {
    const line = fs.readFileSync(file, "utf8").split("\n")[at.line] ?? "";
    const name = /^[A-Za-z_]\w*/.exec(line.slice(at.character))?.[0];
    if (name === undefined) {
        fail(`no identifier at ${at.line}:${at.character} of ${file}; pass --query`);
    }
    return name;
}

/// Locate the CDB the way clice does: workspace root first, then any
/// first-level subdirectory (e.g. build/).
function findCDB(workspace: string): string {
    const candidates = [workspace];
    for (const entry of fs.readdirSync(workspace, { withFileTypes: true })) {
        if (entry.isDirectory()) {
            candidates.push(path.join(workspace, entry.name));
        }
    }
    for (const dir of candidates) {
        const cdb = path.join(dir, "compile_commands.json");
        if (fs.existsSync(cdb)) {
            return cdb;
        }
    }
    fail(`no compile_commands.json under ${workspace} or its direct subdirectories`);
}

function firstCDBEntry(cdbPath: string): string {
    const entries = JSON.parse(fs.readFileSync(cdbPath, "utf8")) as {
        file: string;
        directory?: string;
    }[];
    const first = entries[0];
    if (first === undefined) {
        fail(`empty compile_commands.json at ${cdbPath}`);
    }
    // A relative `directory` anchors at the database's own location, the
    // way the server resolves it.
    const directory = path.resolve(path.dirname(cdbPath), first.directory ?? ".");
    return path.isAbsolute(first.file) ? first.file : path.join(directory, first.file);
}

function nowMs(): number {
    return Number(process.hrtime.bigint()) / 1e6;
}

/// A request bounded like a compile: the client never settles the requests
/// of a server that died under them.
function bounded<T>(request: Promise<T>, what: string): Promise<T> {
    return withTimeout(request, COMPILE_BOUND, what);
}

function pause(ms: number): Promise<void> {
    // No event: an empty answer or a quiet index announces nothing; the
    // caller looks again after this pause.
    return new Promise((resolve) => setTimeout(resolve, ms));
}

/// The directories each server keeps its cache in: clice everything in
/// the workspace's .clice (pinned by the client's initialize); clangd its
/// index under .cache/clangd next to the workspace root and the CDB
/// directory. The rest of a real project's .cache belongs to other tools.
function cacheDirs(opts: Options, server: ServerName): string[] {
    return server === "clice"
        ? [path.join(opts.workspace, ".clice")]
        : [...new Set([opts.workspace, opts.cdbDir])].map((dir) =>
              path.join(dir, ".cache", "clangd"),
          );
}

function wipeCache(opts: Options, server: ServerName): void {
    for (const dir of cacheDirs(opts, server)) {
        fs.rmSync(dir, { recursive: true, force: true });
    }
}

/// The bytes `file` occupies on disk. Windows extends clice's LMDB file to
/// its whole reserved map, sparse: only the ranges fsutil lists as
/// allocated count there. A file the server moves meanwhile counts none.
function diskBytes(file: string): number {
    const size = fs.statSync(file, { throwIfNoEntry: false })?.size ?? 0;
    if (process.platform !== "win32" || size < 64 * 1024 * 1024) {
        return size;
    }
    try {
        const ranges = execFileSync("fsutil", ["sparse", "queryrange", file], { encoding: "utf8" });
        return [...ranges.matchAll(/Length: 0x([0-9a-f]+)/gi)].reduce(
            (sum, match) => sum + Number.parseInt(match[1] ?? "0", 16),
            0,
        );
    } catch {
        // Not a sparse file: all of it is on disk.
        return size;
    }
}

/// Megabytes the server's cache holds on disk, clice's logs aside: all of
/// it, and its index alone. clangd keeps nothing but the index there (its
/// preambles live in memory or in temporary files); clice keeps its index
/// beside the PCHs and PCMs of its store, under cache/<version>/index.
function cacheMB(opts: Options, server: ServerName): { diskMB: number; indexMB: number } {
    const logs = path.join(opts.workspace, ".clice", "logs");
    let disk = 0;
    let index = 0;
    for (const dir of cacheDirs(opts, server)) {
        if (!fs.existsSync(dir)) {
            continue;
        }
        for (const entry of fs.readdirSync(dir, { recursive: true, withFileTypes: true })) {
            const file = path.join(entry.parentPath, entry.name);
            if (!entry.isFile() || file.startsWith(logs + path.sep)) {
                continue;
            }
            const bytes = diskBytes(file);
            disk += bytes;
            const parts = path.relative(dir, file).split(path.sep);
            if (server === "clangd" || (parts[0] === "cache" && parts[2] === "index")) {
                index += bytes;
            }
        }
    }
    const mb = (bytes: number): number => Math.round(bytes / 1024 / 1024);
    return { diskMB: mb(disk), indexMB: mb(index) };
}

/// Drop the OS file cache, so that a cold run reads the project, the
/// system headers and the server itself from disk. Windows has no command
/// for it: its runs stay on a warm file cache.
function dropFileCache(): boolean {
    switch (process.platform) {
        case "linux":
            execFileSync("sudo", ["sh", "-c", "sync && echo 3 > /proc/sys/vm/drop_caches"]);
            return true;
        case "darwin":
            execFileSync("sync");
            execFileSync("sudo", ["purge"]);
            return true;
        default:
            return false;
    }
}

/// What an editor announces: VS Code's capabilities for the features the
/// scenarios ask, work done progress included — clangd reports its
/// background index only to a client that takes it.
const CAPABILITIES: proto.ClientCapabilities = {
    window: { workDoneProgress: true },
    workspace: {
        workspaceEdit: { documentChanges: true },
        symbol: {},
    },
    textDocument: {
        synchronization: { didSave: true },
        hover: { contentFormat: ["markdown", "plaintext"] },
        completion: {
            completionItem: { snippetSupport: true, labelDetailsSupport: true },
            contextSupport: true,
        },
        signatureHelp: {
            signatureInformation: {
                parameterInformation: { labelOffsetSupport: true },
                activeParameterSupport: true,
            },
            contextSupport: true,
        },
        definition: { linkSupport: true },
        references: {},
        publishDiagnostics: { relatedInformation: true },
        documentSymbol: { hierarchicalDocumentSymbolSupport: true },
        foldingRange: { lineFoldingOnly: true },
        semanticTokens: {
            requests: { full: { delta: true } },
            tokenTypes: [],
            tokenModifiers: [],
            formats: ["relative"],
        },
        inlayHint: {},
        callHierarchy: {},
    },
};

/// The servers' background-index work done progress tokens.
const INDEX_TOKENS: Record<ServerName, string> = {
    clice: "clice/backgroundIndex",
    clangd: "backgroundIndexProgress",
};

/// How much an answer holds: the items of a list, the tokens of semantic
/// tokens, the characters of a hover, the signatures of signature help.
function sizeOf(answer: unknown): number {
    if (answer === null || answer === undefined) {
        return 0;
    }
    if (Array.isArray(answer)) {
        return answer.length;
    }
    const record = answer as Record<string, unknown>;
    if (Array.isArray(record["items"])) {
        return record["items"].length;
    }
    if (Array.isArray(record["data"])) {
        return record["data"].length / 5;
    }
    if (Array.isArray(record["signatures"])) {
        return record["signatures"].length;
    }
    if (record["contents"] !== undefined) {
        return JSON.stringify(record["contents"]).length;
    }
    return 1;
}

/// Document symbols counted through their children.
function symbolCount(answer: unknown): number {
    if (!Array.isArray(answer)) {
        return 0;
    }
    return (answer as { children?: unknown }[]).reduce(
        (sum: number, symbol) => sum + 1 + symbolCount(symbol.children),
        0,
    );
}

/// The distinct files references reach.
function filesOf(answer: unknown): number {
    return Array.isArray(answer) ? new Set(answer.map((at: { uri: string }) => at.uri)).size : 0;
}

/// One run of one scenario by one server: its server process, the memory
/// watch over it, and what it measured.
class Run {
    readonly result: RunResult;
    readonly opts: Options;
    client: CliceClient | null = null;
    private memory: { stop: () => WatchedMemory } | null = null;
    /// Log files that existed before this run's server started; the files
    /// beyond them are exactly this run's session logs.
    private priorLogs = new Set<string>();
    /// When set (epoch ms), server-side perf events timestamped before this
    /// point are dropped: they belong to setup/warmup work the client
    /// series deliberately excludes.
    perfWindowStart: number | null = null;
    /// When the server process was spawned.
    started = 0;

    constructor(opts: Options, server: ServerName, scenario: ScenarioName, run: number) {
        this.opts = opts;
        this.result = { server, scenario, run, series: {}, sizes: {} };
    }

    get server(): ServerName {
        return this.result.server;
    }

    record(name: string, ms: number): void {
        (this.result.series[name] ??= []).push(ms);
    }

    async measure(name: string, fn: () => Promise<unknown>): Promise<unknown> {
        const start = nowMs();
        const answer = await bounded(fn(), name);
        this.record(name, nowMs() - start);
        return answer;
    }

    async start(): Promise<CliceClient> {
        const opts = this.opts;
        if (this.server === "clice") {
            this.priorLogs = new Set(logFiles(opts.workspace));
        }
        this.started = nowMs();
        const client = CliceClient.start(opts.binaries[this.server], {
            args:
                this.server === "clice"
                    ? ["serve"]
                    : [`--compile-commands-dir=${opts.cdbDir}`, ...opts.clangdArgs],
            // A relative CDB `directory` anchors at the database for clice but
            // at the process cwd for clangd; running clangd from the CDB
            // directory makes both servers compile under the same command.
            cwd: this.server === "clangd" ? opts.cdbDir : undefined,
            checked: false,
        });
        this.client = client;
        if (client.child.pid !== undefined) {
            this.memory = watchMemory(client.child.pid);
        }
        await client.initialize(new Workspace(opts.workspace), {
            initializationOptions: {
                // Pin clice to the CDB selected for the comparison: a
                // workspace clice.toml may declare databases elsewhere, while
                // clangd always receives opts.cdbDir — both must open the file
                // under the same compilation command. The overlay replaces the
                // file's rules wholesale, so none of its flag edits apply
                // either, matching clangd.
                rules: [{ compile_commands: [opts.cdbDir] }],
                project: {
                    // Pin logs to where finish() reads them (logFiles searches
                    // <workspace>/.clice/logs); a clice.toml logging_dir would
                    // otherwise send the worker perf lines elsewhere.
                    logging_dir: path.join(opts.workspace, ".clice", "logs"),
                },
            },
            testDefaults: false,
            capabilities: CAPABILITIES,
        });
        this.record("initialize", nowMs() - this.started);
        return client;
    }

    /// Asks until an answer holds something; the time from `from` to that
    /// answer is the series `name`. An empty answer to a question asked
    /// once `settled` holds is the server's final word: the series gets no
    /// sample, and the size records the empty answer.
    async untilAnswered(
        name: string,
        from: number,
        ask: () => Promise<unknown>,
        settled: () => boolean,
        size: (answer: unknown) => number = sizeOf,
    ): Promise<void> {
        for (;;) {
            const final = settled();
            const held = size(await bounded(ask(), name));
            if (held > 0) {
                this.record(name, nowMs() - from);
                this.result.sizes[name] = held;
                return;
            }
            if (final) {
                this.result.sizes[name] = 0;
                console.warn(`${this.server}: ${name} answered empty`);
                return;
            }
            await pause(50);
        }
    }

    /// Waits until the background index is idle: no round running and none
    /// begun for INDEX_QUIET_MS. With `round`, a round must have ended
    /// first. Returns when the last round ended, and the most files a round
    /// counted.
    async indexIdle(round: boolean): Promise<{ ended: number; files: number }> {
        const client = this.client;
        if (client === null) {
            throw new Error("the server is not running");
        }
        const token = INDEX_TOKENS[this.server];
        let seen = 0;
        let changed = nowMs();
        let active = false;
        let ended: number | null = null;
        let files = 0;
        while (nowMs() - this.started < INDEX_BOUND) {
            if (client.child.exitCode !== null || client.child.signalCode !== null) {
                throw new Error(`${this.server} exited while its background index was awaited`);
            }
            const events = client.progressEvents;
            for (; seen < events.length; seen += 1) {
                const event = events[seen];
                if (event?.token !== token) {
                    continue;
                }
                const value = event.value as { kind?: string; message?: string };
                changed = nowMs();
                active = value.kind !== "end";
                if (value.kind === "end") {
                    ended = changed;
                }
                const counted = /(\d+)\/(\d+)/.exec(value.message ?? "");
                if (counted?.[2] !== undefined) {
                    files = Math.max(files, Number(counted[2]));
                }
            }
            if (!active && (!round || ended !== null) && nowMs() - changed >= INDEX_QUIET_MS) {
                return { ended: ended ?? changed, files };
            }
            await pause(250);
        }
        throw new Error(`${this.server}: the background index is not idle after ${INDEX_BOUND} ms`);
    }

    async finish(): Promise<RunResult> {
        const client = this.client;
        if (client === null) {
            return this.result;
        }
        try {
            const memory = this.memory?.stop();
            if (memory !== undefined) {
                this.result.memory = memory;
            }
            if (this.server === "clice") {
                this.collectPerf(client);
            }
        } finally {
            await shutdownServer(client, this.server);
        }
        return this.result;
    }

    /// Server-side breakdown from the session's log files — the worker
    /// processes' `[perf:build]`/`[perf:query]` lines only exist there
    /// (workers never mirror to stderr). Captured stderr, which carries the
    /// master's lines, is the fallback when no log files appeared.
    private collectPerf(client: CliceClient): void {
        const fresh = logFiles(this.opts.workspace).filter((f) => !this.priorLogs.has(f));
        // One pid per log file: master and workers are separate processes
        // and keep separate trace lanes.
        let events =
            fresh.length > 0
                ? fresh.flatMap((f, i) => parsePerfLines(fs.readFileSync(f, "utf8"), i + 1))
                : parsePerfLines(client.drainedStderr().toString("utf8"));
        const windowStart = this.perfWindowStart;
        if (windowStart !== null) {
            events = events.filter((e) => e.ts !== null && e.ts >= windowStart);
        }
        const perf = summarize(events);
        if (Object.keys(perf).length > 0) {
            this.result.perf = perf;
        }
    }
}

async function shutdownServer(client: CliceClient, server: ServerName): Promise<void> {
    if (server === "clice") {
        await client.shutdown();
        return;
    }
    // clangd exits on its own protocol; skip clice's clean-exit gate
    // (drop report, anomaly scan) which does not apply to it.
    try {
        await client.sendRequest("shutdown");
        await client.sendNotification("exit");
    } finally {
        client.disposed = true;
        // Hang detection: a clangd still running after `exit` is killed.
        setTimeout(() => {
            client.child.kill("SIGKILL");
        }, 60_000).unref();
        await client.exited;
    }
}

function symbolProbe(opts: Options): proto.Position {
    const symbol = opts.probes.symbol;
    if (symbol === undefined) {
        throw new Error("main() derives the symbol probe");
    }
    return symbol;
}

/// Server start → the opened file's first diagnostics, and the first
/// non-empty semantic tokens, hover and definition at the symbol probe,
/// asked right at the open as an editor does; a warm start also times the
/// first references reaching a second file — the saved index loaded, as
/// clangd's index of the open file holds no other file's references. The
/// open's samples count from the didOpen.
async function runStart(run: Run, warm: boolean): Promise<void> {
    const opts = run.opts;
    const client = await run.start();
    const { line, character } = symbolProbe(opts);
    const opened = nowMs();
    const [uri] = client.open(opts.file);
    let built = false;
    let indexed = false;
    const afterBuild = (): boolean => built;
    const waits = [
        bounded(client.armDiagnostics(uri), `diagnostics ${uri}`).then(() => {
            run.record("open_to_diagnostics", nowMs() - opened);
            built = true;
        }),
        run.untilAnswered(
            "open_to_semantic_tokens",
            opened,
            () => client.semanticTokensFull(uri),
            afterBuild,
        ),
        run.untilAnswered(
            "open_to_hover",
            opened,
            () => client.hoverAt(uri, line, character),
            afterBuild,
        ),
        run.untilAnswered(
            "open_to_definition",
            opened,
            () => client.definitionAt(uri, line, character),
            afterBuild,
        ),
    ];
    if (warm) {
        const afterLoad = (): boolean => built && indexed;
        waits.push(
            run.indexIdle(false).then(() => {
                indexed = true;
            }),
            run.untilAnswered(
                "open_to_cross_file_references",
                opened,
                () => client.referencesAt(uri, line, character),
                afterLoad,
                (answer) => (filesOf(answer) >= 2 ? sizeOf(answer) : 0),
            ),
        );
    }
    await Promise.all(waits);
}

/// Server start → the background index of the whole project idle, the
/// focus file open as the editor that started the server has it (clangd
/// finds its database through an opened file).
async function runIndex(run: Run): Promise<void> {
    const opts = run.opts;
    const client = await run.start();
    const [uri] = client.open(opts.file);
    await bounded(client.semanticTokensFull(uri), `semantic tokens ${uri}`);
    const { ended, files } = await run.indexIdle(true);
    run.record("index", ended - run.started);
    // Sized once the server is down: clice saves its index after the round
    // ends, and again at exit.
    run.result.index = { files, indexMB: 0, diskMB: 0 };
}

/// Opens `file` and waits for its first diagnostics, the build pulled by
/// the semantic tokens an editor asks at once.
async function openBuilt(client: CliceClient, file: string): Promise<[string, string]> {
    const opened = client.open(file);
    const [uri] = opened;
    const built = client.armDiagnostics(uri);
    await bounded(client.semanticTokensFull(uri), `semantic tokens ${uri}`);
    await bounded(built, `diagnostics ${uri}`);
    return opened;
}

async function runEditLoop(run: Run): Promise<void> {
    const opts = run.opts;
    const client = await run.start();
    const [uri, content] = await openBuilt(client, opts.file);

    // Append at the end of the TU: every edit invalidates the opened file
    // but not the preamble — the interactive path the server optimizes.
    // Sent as a ranged insertion like a real editor; a full-document
    // replacement would add whole-file serialization to every sample.
    const lines = content.split("\n");
    let end = { line: lines.length - 1, character: lines[lines.length - 1]?.length ?? 0 };

    for (let i = 1; i <= opts.edits; i += 1) {
        const comment = `// bench edit ${i}`;
        const edited = nowMs();
        const arrived = client.armDiagnostics(uri);
        client.changeRange(uri, i, { start: end, end }, `\n${comment}`);
        // The semantic tokens an editor asks after every change; they also
        // pull clice's rebuild.
        await Promise.all([
            bounded(arrived, `diagnostics ${uri}`).then(() => {
                run.record("edit_to_diagnostics", nowMs() - edited);
            }),
            bounded(client.semanticTokensFull(uri), `semantic tokens ${uri}`).then(() => {
                run.record("edit_to_semantic_tokens", nowMs() - edited);
            }),
        ]);
        end = { line: end.line + 1, character: comment.length };
    }
}

async function runWarmRequests(run: Run): Promise<void> {
    const opts = run.opts;
    const client = await run.start();
    const [uri] = await openBuilt(client, opts.file);
    // References and the call hierarchy read the whole index: it must be
    // loaded first, as it is in a session that has run a while.
    await run.indexIdle(false);

    const { line, character } = symbolProbe(opts);
    const shown = {
        start: { line: Math.max(0, line - 50), character: 0 },
        end: { line: line + 50, character: 0 },
    };
    const requests: Record<string, () => Promise<unknown>> = {
        hover: () => client.hoverAt(uri, line, character),
        definition: () => client.definitionAt(uri, line, character),
        references: () => client.referencesAt(uri, line, character),
        call_hierarchy: async () => {
            const items = await client.prepareCallHierarchy(uri, line, character);
            const item = items?.[0];
            return item === undefined ? [] : client.callHierarchyIncoming(item);
        },
        workspace_symbol: () => client.workspaceSymbols(opts.query),
        document_symbol: () => client.documentSymbols(uri),
        semantic_tokens: () => client.semanticTokensFull(uri),
        folding_range: () => client.foldingRanges(uri),
        inlay_hint: () => client.inlayHints(uri, shown),
    };
    const completions: [string, ProbeKind][] = [
        ["completion_member", "member"],
        ["completion_unqualified", "unqualified"],
        ["completion_template", "template"],
    ];
    for (const [name, kind] of completions) {
        const at = opts.probes[kind];
        if (at !== undefined) {
            requests[name] = () => client.completionAt(uri, at.line, at.character);
        }
    }
    const call = opts.probes.call;
    if (call !== undefined) {
        requests["signature_help"] = () => client.signatureHelpAt(uri, call.line, call.character);
    }

    // One unmeasured warmup per request absorbs lazy first-request work.
    // All warmups run before the measurement window opens, so the
    // server-side series carry exactly --repeats events per kind.
    for (const request of Object.values(requests)) {
        await request();
    }
    // Log timestamps truncate to milliseconds, so a boundary read in the
    // same millisecond as the last warmup's perf line cannot separate the
    // two. Park until the wall clock leaves the warmup millisecond: every
    // warmup event is timestamped <= boundary, every measured one >= boundary+1.
    const boundary = Date.now();
    while (Date.now() <= boundary) {
        // Time is the feature: the wait is for the clock itself.
        await new Promise((resolve) => setTimeout(resolve, 1));
    }
    run.perfWindowStart = boundary + 1;

    for (const [name, request] of Object.entries(requests)) {
        let answer: unknown = null;
        for (let i = 0; i < opts.repeats; i += 1) {
            answer = await run.measure(name, request);
        }
        run.result.sizes[name] = name === "document_symbol" ? symbolCount(answer) : sizeOf(answer);
    }
}

async function runScenario(
    opts: Options,
    server: ServerName,
    scenario: ScenarioName,
    number: number,
): Promise<RunResult> {
    const run = new Run(opts, server, scenario, number);
    if (scenario === "cold_start" || scenario === "index") {
        wipeCache(opts, server);
        if (opts.dropCaches) {
            run.result.dropped = dropFileCache();
        }
    }
    // A run that fails is a result too: the session goes on to the others.
    try {
        switch (scenario) {
            case "cold_start":
                await runStart(run, false);
                break;
            case "warm_start":
                await runStart(run, true);
                break;
            case "index":
                await runIndex(run);
                break;
            case "edit_loop":
                await runEditLoop(run);
                break;
            case "warm_requests":
                await runWarmRequests(run);
                break;
        }
    } catch (error) {
        run.result.error = String(error);
    }
    try {
        await run.finish();
    } catch (error) {
        run.result.error ??= String(error);
    }
    if (run.result.index !== undefined) {
        Object.assign(run.result.index, cacheMB(opts, server));
    }
    return run.result;
}

function printRun(result: RunResult): void {
    const parts = Object.entries(result.series).map(([name, values]) => {
        const stats = computeStats(values);
        return stats === null ? name : `${name}=${stats.p50.toFixed(1)}`;
    });
    if (result.memory !== undefined) {
        parts.push(`peak_mb=${result.memory.peak.total}`);
        parts.push(`end_mb=${result.memory.end.root + result.memory.end.children}`);
    }
    if (result.index !== undefined) {
        parts.push(
            `files=${result.index.files}`,
            `index_mb=${result.index.indexMB}`,
            `disk_mb=${result.index.diskMB}`,
        );
    }
    if (result.error !== undefined) {
        parts.push(`failed: ${result.error}`);
    }
    console.log(`[${result.scenario} #${result.run} ${result.server}] ${parts.join(" ")}`);
}

async function main(): Promise<void> {
    const opts = parseOptions();
    const cdb = findCDB(opts.workspace);
    opts.cdbDir = path.dirname(cdb);
    opts.file = opts.file !== "" ? path.resolve(opts.workspace, opts.file) : firstCDBEntry(cdb);
    opts.probes.symbol ??= derivePosition(opts.file);
    if (opts.query === "") {
        opts.query = identifierAt(opts.file, opts.probes.symbol);
    }
    const cpus = os.cpus();

    const result: BenchResult = {
        env: {
            platform: process.platform,
            release: os.release(),
            arch: process.arch,
            cpus: cpus.length,
            cpuModel: cpus[0]?.model ?? "unknown",
            memoryMB: Math.round(os.totalmem() / 1024 / 1024),
            node: process.version,
            date: new Date().toISOString(),
        },
        binaries: Object.fromEntries(opts.servers.map((server) => [server, opts.binaries[server]])),
        clangdArgs: opts.clangdArgs,
        workspace: opts.workspace,
        file: opts.file,
        probes: opts.probes,
        query: opts.query,
        runs: [],
    };

    console.log(`servers: ${opts.servers.map((s) => `${s} (${opts.binaries[s]})`).join(", ")}`);
    console.log(`workspace: ${opts.workspace}`);
    console.log(`file: ${opts.file}`);
    console.log(`probes: ${JSON.stringify(opts.probes)}, query: ${opts.query}`);

    const write = (): void => {
        if (opts.jsonPath !== null) {
            fs.writeFileSync(opts.jsonPath, JSON.stringify(result, null, 2));
        }
    };
    // The servers alternate run by run: a drift of the machine over the
    // session reaches both alike.
    for (const scenario of opts.scenarios) {
        for (let number = 1; number <= opts.runs; number += 1) {
            for (const server of opts.servers) {
                const run = await runScenario(opts, server, scenario, number);
                result.runs.push(run);
                printRun(run);
                write();
            }
        }
    }
    if (opts.jsonPath !== null) {
        console.log(`\nresults written to ${opts.jsonPath}`);
    }
}

await main();
