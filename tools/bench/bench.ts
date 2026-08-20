/// E2E benchmark harness: drive a language server over LSP through fixed
/// scenarios on a real workspace and report client-observed latencies.
///
/// The harness is server-agnostic at the LSP level, so the same scenarios
/// run against clice and clangd for A/B comparison. For clice it
/// additionally parses the session's `[perf:*]` log lines — master and
/// worker files under <workspace>/.clice/logs — into a server-side
/// breakdown (see perf.ts).
///
/// Usage:
///   node tools/bench/bench.ts --workspace <dir> [options]
///
/// Options:
///   --server clice|clangd    which server to drive (default clice)
///   --binary <path>          server executable (default: clice from
///                            build/RelWithDebInfo/bin, clangd from PATH)
///   --file <rel>             file to open/edit (default: first CDB entry)
///   --position <line:char>   position for warm requests (default: derived
///                            from the file's first call-like identifier)
///   --scenario <name>        cold_start | warm_start | edit_loop |
///                            warm_requests; repeatable (default: all)
///   --edits <N>              edit_loop iterations (default 10)
///   --repeats <N>            warm request repetitions (default 50)
///   --json <path>            write the full results as JSON
///
/// The workspace must contain a compile_commands.json. cold_start wipes
/// the selected server's cache first (clice's .clice dir, clangd's
/// .cache/clangd); the other scenarios reuse whatever state the previous
/// scenarios left, in order.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { parseArgs } from "node:util";
import { fileURLToPath } from "node:url";
import { CliceClient, logFiles, withTimeout } from "../client/client.ts";
import { Workspace } from "../client/workspace.ts";
import { computeStats, parsePerfLines, summarize, type Stats } from "./perf.ts";

const REPO_ROOT = path.dirname(path.dirname(path.dirname(fileURLToPath(import.meta.url))));

const ALL_SCENARIOS = ["cold_start", "warm_start", "edit_loop", "warm_requests"] as const;
type ScenarioName = (typeof ALL_SCENARIOS)[number];

interface Options {
    server: "clice" | "clangd";
    binary: string;
    workspace: string;
    /// Directory holding compile_commands.json; resolved in main().
    cdbDir: string;
    file: string | null;
    /// null = derive a symbol-bearing position from the file content.
    position: { line: number; character: number } | null;
    scenarios: ScenarioName[];
    edits: number;
    repeats: number;
    jsonPath: string | null;
}

interface ScenarioResult {
    /// Client-observed latency series, milliseconds.
    measurements: Record<string, Stats>;
    /// Server-side breakdown from the session's `[perf:*]` log lines
    /// (clice only).
    perf?: Record<string, Stats>;
}

interface BenchResult {
    env: {
        platform: string;
        release: string;
        arch: string;
        cpus: number;
        cpuModel: string;
        node: string;
        server: string;
        binary: string;
        date: string;
    };
    workspace: string;
    file: string;
    scenarios: Partial<Record<ScenarioName, ScenarioResult>>;
}

function fail(message: string): never {
    console.error(`error: ${message}`);
    process.exit(1);
}

function parseOptions(): Options {
    const { values } = parseArgs({
        options: {
            server: { type: "string", default: "clice" },
            binary: { type: "string" },
            workspace: { type: "string" },
            file: { type: "string" },
            position: { type: "string" },
            scenario: { type: "string", multiple: true },
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

    const server = values.server;
    if (server !== "clice" && server !== "clangd") {
        fail(`unknown server '${server}' (expected clice or clangd)`);
    }

    const binary =
        values.binary ??
        (server === "clice"
            ? path.join(REPO_ROOT, "build", "RelWithDebInfo", "bin", "clice")
            : "clangd");

    const scenarios = (values.scenario ?? [...ALL_SCENARIOS]).map((name) => {
        if (!(ALL_SCENARIOS as readonly string[]).includes(name)) {
            fail(`unknown scenario '${name}'`);
        }
        return name as ScenarioName;
    });

    let position: { line: number; character: number } | null = null;
    if (values.position !== undefined) {
        const parts = values.position.split(":");
        const line = Number(parts[0]);
        const character = Number(parts[1]);
        if (
            parts.length !== 2 ||
            !Number.isInteger(line) ||
            line < 0 ||
            !Number.isInteger(character) ||
            character < 0
        ) {
            fail(`bad --position '${values.position}' (expected line:char)`);
        }
        position = { line, character };
    }

    const edits = Number(values.edits);
    const repeats = Number(values.repeats);
    if (!Number.isInteger(edits) || edits < 0 || !Number.isInteger(repeats) || repeats < 0) {
        fail("--edits and --repeats must be non-negative integers");
    }

    return {
        server,
        binary,
        workspace: path.resolve(values.workspace),
        cdbDir: "",
        file: values.file ?? null,
        position,
        scenarios,
        edits,
        repeats,
        jsonPath: values.json ?? null,
    };
}

/// Overwrite comment and string/char literal bytes with spaces, keeping
/// offsets and newlines intact, so position derivation only ever sees
/// code. Raw string literals are treated as ordinary strings — good
/// enough for a probe heuristic with a --position override.
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
function derivePosition(file: string): { line: number; character: number } {
    const keywords = new Set(["if", "for", "while", "switch", "return", "sizeof", "catch"]);
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
    fail(`cannot derive a symbol-bearing position in ${file}; pass --position`);
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
    return path.isAbsolute(first.file)
        ? first.file
        : path.join(first.directory ?? path.dirname(cdbPath), first.file);
}

function nowMs(): number {
    return Number(process.hrtime.bigint()) / 1e6;
}

async function timed(fn: () => Promise<unknown>): Promise<number> {
    const start = nowMs();
    await fn();
    return nowMs() - start;
}

class Bench {
    series = new Map<string, number[]>();
    /// Log files that existed before this scenario's server ran; every
    /// scenario spawns a fresh server, so files beyond this set are exactly
    /// this scenario's session logs.
    priorLogs: Set<string>;
    /// When set (epoch ms), server-side perf events stamped before this
    /// point are dropped: they belong to setup/warmup work the client
    /// series deliberately excludes.
    perfWindowStart: number | null = null;
    opts: Options;

    constructor(opts: Options) {
        this.opts = opts;
        this.priorLogs = new Set(opts.server === "clice" ? logFiles(opts.workspace) : []);
    }

    record(name: string, ms: number): void {
        const list = this.series.get(name) ?? [];
        list.push(ms);
        this.series.set(name, list);
    }

    async measure(name: string, fn: () => Promise<unknown>): Promise<void> {
        this.record(name, await timed(fn));
    }

    /// Server-side breakdown from the session's log files — the worker
    /// processes' `[perf:build]`/`[perf:query]` lines only exist there
    /// (workers never mirror to stderr). Captured stderr, which carries the
    /// master's lines, is the fallback when no log files appeared.
    result(client: CliceClient): ScenarioResult {
        const measurements: Record<string, Stats> = {};
        for (const [name, values] of this.series) {
            const stats = computeStats(values);
            if (stats !== null) {
                measurements[name] = stats;
            }
        }
        const result: ScenarioResult = { measurements };
        if (this.opts.server === "clice") {
            const fresh = logFiles(this.opts.workspace).filter((f) => !this.priorLogs.has(f));
            // One pid per log file: master and workers are separate
            // processes and keep separate trace lanes.
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
                result.perf = perf;
            }
        }
        return result;
    }
}

/// clice takes the CDB via initialization options; clangd needs the CDB
/// directory spelled out on the command line.
function serverArgs(opts: Options): string[] {
    return opts.server === "clice"
        ? ["serve"]
        : ["--background-index", `--compile-commands-dir=${opts.cdbDir}`];
}

/// CliceClient.initialize defaults worker counts to 1 and zeroes the
/// tracker polling loops for cheap deterministic tests; a benchmark must
/// run the server's real defaults (stateful 2, stateless cores/2, tracker
/// 3s/30s, from src/server/state/config.h) including the background
/// activity those loops generate.
function initializationOptions(opts: Options): Record<string, unknown> {
    return {
        project: {
            // Pin clice to the CDB selected for the comparison: a workspace
            // clice.toml may configure compile_commands_paths elsewhere,
            // while the clangd run always receives opts.cdbDir — the A/B
            // must open the file under the same compilation command.
            compile_commands_paths: [opts.cdbDir],
            // Pin logs to where result() reads them (logFiles searches
            // <workspace>/.clice/logs); a clice.toml logging_dir would
            // otherwise send the worker perf lines elsewhere.
            logging_dir: path.join(opts.workspace, ".clice", "logs"),
            stateful_worker_count: 2,
            // availableParallelism respects cpusets and container CPU
            // quotas; os.cpus() is the host's full list.
            stateless_worker_count: Math.max(Math.floor(os.availableParallelism() / 2), 2),
        },
        tracker: {
            cdb_poll_seconds: 3,
            workspace_poll_seconds: 30,
        },
    };
}

async function startServer(opts: Options): Promise<CliceClient> {
    const client = CliceClient.start(opts.binary, { args: serverArgs(opts) });
    await client.initialize(new Workspace(opts.workspace), {
        initializationOptions: initializationOptions(opts),
    });
    return client;
}

async function shutdownServer(client: CliceClient, opts: Options): Promise<void> {
    if (opts.server === "clice") {
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
        setTimeout(() => {
            client.child.kill("SIGKILL");
        }, 5_000).unref();
        await client.exited;
    }
}

/// Server start → initialize response → first diagnostics of the main file.
///
/// The hover poke that triggers compilation on clice stays off the timed
/// path: diagnostics publish when compilation settles and the hover is
/// computed afterward, so awaiting its response first (as openAndWait
/// does) would fold hover computation into every sample. The sample ends
/// at the diagnostics notification; the poke is drained after the clock
/// stops.
async function runStartScenario(opts: Options, file: string): Promise<ScenarioResult> {
    const bench = new Bench(opts);

    const start = nowMs();
    const client = await startServer(opts);
    bench.record("initialize", nowMs() - start);

    let poke: Promise<unknown> = Promise.resolve();
    await bench.measure("open_to_diagnostics", async () => {
        const [uri] = client.open(file);
        const arrived = client.armDiagnostics(uri);
        poke = client.hoverAt(uri, 0, 0);
        await withTimeout(arrived, 300_000, `diagnostics ${uri}`);
    });
    await poke;

    const result = bench.result(client);
    await shutdownServer(client, opts);
    return result;
}

async function runColdStart(opts: Options, file: string): Promise<ScenarioResult> {
    // Wipe only the selected server's cache: clice keeps everything in the
    // workspace's .clice (pinned by the client's initialize); clangd keeps
    // its index under .cache/clangd next to the workspace root and the CDB
    // directory. The rest of a real project's .cache belongs to other
    // tools and must survive.
    if (opts.server === "clice") {
        fs.rmSync(path.join(opts.workspace, ".clice"), { recursive: true, force: true });
    } else {
        for (const dir of new Set([opts.workspace, opts.cdbDir])) {
            fs.rmSync(path.join(dir, ".cache", "clangd"), { recursive: true, force: true });
        }
    }
    return runStartScenario(opts, file);
}

async function runEditLoop(opts: Options, file: string): Promise<ScenarioResult> {
    const bench = new Bench(opts);
    const client = await startServer(opts);
    const [uri, content] = await client.openAndWait(file, 300_000);

    // Append at the end of the TU: every edit invalidates the main file
    // but not the preamble — the interactive path the server optimizes.
    // Sent as a ranged insertion like a real editor; a full-document
    // replacement would add whole-file serialization to every sample.
    const lines = content.split("\n");
    let end = { line: lines.length - 1, character: lines[lines.length - 1]?.length ?? 0 };

    for (let i = 1; i <= opts.edits; i += 1) {
        const comment = `// bench edit ${i}`;
        // See runStartScenario: the sample ends at the diagnostics
        // notification, with the triggering hover drained off the clock.
        let poke: Promise<unknown> = Promise.resolve();
        await bench.measure("edit_to_diagnostics", async () => {
            const arrived = client.armDiagnostics(uri);
            client.changeRange(uri, i, { start: end, end }, `\n${comment}`);
            poke = client.hoverAt(uri, 0, 0);
            await withTimeout(arrived, 300_000, `diagnostics ${uri}`);
        });
        await poke;
        end = { line: end.line + 1, character: comment.length };
    }

    const result = bench.result(client);
    await shutdownServer(client, opts);
    return result;
}

async function runWarmRequests(opts: Options, file: string): Promise<ScenarioResult> {
    const bench = new Bench(opts);
    const client = await startServer(opts);
    const [uri] = await client.openAndWait(file, 300_000);
    const position = opts.position ?? derivePosition(file);
    if (opts.position === null) {
        console.log(`derived probe position ${position.line}:${position.character}`);
    }
    const { line, character } = position;

    const requests: Record<string, () => Promise<unknown>> = {
        hover: () => client.hoverAt(uri, line, character),
        definition: () => client.definitionAt(uri, line, character),
        references: () => client.referencesAt(uri, line, character),
        completion: () => client.completionAt(uri, line, character),
        document_symbol: () => client.documentSymbols(uri),
        semantic_tokens: () => client.semanticTokensFull(uri),
    };

    // One unmeasured warmup per request absorbs lazy first-request work.
    // All warmups run before the measurement window opens, so the
    // server-side series carry exactly --repeats events per kind.
    for (const request of Object.values(requests)) {
        await request();
    }
    // Log timestamps truncate to milliseconds, so a boundary read in the
    // same millisecond as the last warmup's perf line cannot separate the
    // two. Park until the wall clock leaves the warmup millisecond: every
    // warmup event is stamped <= boundary, every measured one >= boundary+1.
    const boundary = Date.now();
    while (Date.now() <= boundary) {
        await new Promise((resolve) => setTimeout(resolve, 1));
    }
    bench.perfWindowStart = boundary + 1;

    for (const [name, request] of Object.entries(requests)) {
        for (let i = 0; i < opts.repeats; i += 1) {
            await bench.measure(name, request);
        }
    }

    const result = bench.result(client);
    await shutdownServer(client, opts);
    return result;
}

function printScenario(name: string, result: ScenarioResult): void {
    console.log(`\n=== ${name} ===`);
    const rows = Object.entries(result.measurements);
    console.log(
        `  ${"series".padEnd(24)} ${"count".padStart(6)} ${"p50".padStart(9)} ` +
            `${"p90".padStart(9)} ${"p99".padStart(9)} ${"max".padStart(9)}`,
    );
    for (const [series, stats] of rows) {
        console.log(
            `  ${series.padEnd(24)} ${String(stats.count).padStart(6)} ` +
                `${stats.p50.toFixed(2).padStart(9)} ${stats.p90.toFixed(2).padStart(9)} ` +
                `${stats.p99.toFixed(2).padStart(9)} ${stats.max.toFixed(2).padStart(9)}`,
        );
    }
    if (result.perf !== undefined) {
        console.log("  server-side breakdown ([perf:*], ms):");
        for (const [series, stats] of Object.entries(result.perf)) {
            console.log(
                `    ${series.padEnd(38)} n=${String(stats.count).padStart(5)} ` +
                    `p50=${stats.p50.toFixed(2).padStart(9)} max=${stats.max.toFixed(2).padStart(9)}`,
            );
        }
    }
}

async function main(): Promise<void> {
    const opts = parseOptions();
    const cdb = findCDB(opts.workspace);
    opts.cdbDir = path.dirname(cdb);
    const file = opts.file !== null ? path.resolve(opts.workspace, opts.file) : firstCDBEntry(cdb);
    const cpus = os.cpus();

    const result: BenchResult = {
        env: {
            platform: process.platform,
            release: os.release(),
            arch: process.arch,
            cpus: cpus.length,
            cpuModel: cpus[0]?.model ?? "unknown",
            node: process.version,
            server: opts.server,
            binary: opts.binary,
            date: new Date().toISOString(),
        },
        workspace: opts.workspace,
        file,
        scenarios: {},
    };

    console.log(`server: ${opts.server} (${opts.binary})`);
    console.log(`workspace: ${opts.workspace}`);
    console.log(`file: ${file}`);

    const runners: Record<ScenarioName, () => Promise<ScenarioResult>> = {
        cold_start: () => runColdStart(opts, file),
        warm_start: () => runStartScenario(opts, file),
        edit_loop: () => runEditLoop(opts, file),
        warm_requests: () => runWarmRequests(opts, file),
    };

    for (const name of opts.scenarios) {
        const scenario = await runners[name]();
        result.scenarios[name] = scenario;
        printScenario(name, scenario);
    }

    if (opts.jsonPath !== null) {
        fs.writeFileSync(opts.jsonPath, JSON.stringify(result, null, 2));
        console.log(`\nresults written to ${opts.jsonPath}`);
    }
}

await main();
