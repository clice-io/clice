/// One run of a perf scenario: one server process from its start to its
/// exit, the requests the scenario times on it, the memory and CPU of its
/// process tree, and for clice the breakdown its `[perf:*]` log lines give.

import * as fs from "node:fs";
import * as path from "node:path";
import * as proto from "vscode-languageserver-protocol";
import { CliceClient } from "../../client/client.ts";
import { Workspace } from "../../client/workspace.ts";
import { ANOMALY_PATTERN, logFiles } from "../../process_gate.ts";
import { withTimeout } from "../../promise.ts";
import { CRASHED_IN } from "../../testing/server/serve.ts";
import { watchTree, type TreeWatch } from "../memory.ts";
import { computeStats, parsePerfLines, summarize } from "./logs.ts";
import type { RunResult, ServerName } from "./results.ts";
import { CAPABILITIES, cliceDir, indexToken, isClice } from "./servers.ts";

/// What every run of a job shares: the checkout, the database's directory,
/// the build directory whose generated files no scenario changes, the
/// servers, and the time (epoch ms) past which no scenario begins and no
/// wait goes on.
export interface PerfContext {
    workspace: string;
    database: string;
    build?: string;
    binaries: Partial<Record<ServerName, string>>;
    clangdArgs: readonly string[];
    dropCaches: boolean;
    deadline: number;
}

/// Hang detection: a cold compile of a large project's largest file
/// (LLVM's SemaExpr.cpp) on a 3-core runner, with room to spare.
const REQUEST_BOUND = 600_000;
/// Hang detection: a full background index of LLVM on a 3-core runner.
const INDEX_BOUND = 6 * 3600_000;
/// How long the servers must stay quiet to count as idle: no index
/// progress, and a tenth of a core at most. Both may end one round and
/// begin another for files found meanwhile.
const QUIET_MS = 15_000;
/// Hang detection: the longest a server takes to announce its first round
/// on a wiped cache. clice drops a round's progress when the round ends
/// before the client acknowledged its token (a project of a few files).
const ANNOUNCE_BOUND = 300_000;

export function nowMs(): number {
    return Number(process.hrtime.bigint()) / 1e6;
}

/// A request bounded like a compile: the client never settles the requests
/// of a server that died under them.
export function bounded<T>(request: Promise<T>, what: string, ms = REQUEST_BOUND): Promise<T> {
    return withTimeout(request, ms, what);
}

export function pause(ms: number): Promise<void> {
    // No event: an empty answer, a quiet index or an idle server announces
    // nothing; the caller looks again after this pause.
    return new Promise((resolve) => setTimeout(resolve, ms));
}

export function median(values: readonly number[]): number {
    return computeStats([...values])?.p50 ?? 0;
}

/// How much an answer holds: the items of a list, the tokens of semantic
/// tokens, the characters of a hover, the signatures of signature help.
export function sizeOf(answer: unknown): number {
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

export class Session {
    readonly result: RunResult;
    readonly context: PerfContext;
    private connected: CliceClient | null = null;
    private watch: TreeWatch | null = null;
    /// Log files that existed before this run's server started; the files
    /// beyond them are exactly this run's session logs.
    private priorLogs = new Set<string>();
    /// When set (epoch ms), server-side perf events timestamped before this
    /// point are dropped: they belong to warmup work the client series
    /// deliberately exclude.
    perfWindowStart: number | null = null;
    /// When the server process was spawned.
    started = 0;
    private readonly versions = new Map<string, number>();
    private readonly ends = new Map<string, proto.Position>();

    constructor(context: PerfContext, result: RunResult) {
        this.context = context;
        this.result = result;
    }

    get server(): ServerName {
        return this.result.server;
    }

    get client(): CliceClient {
        if (this.connected === null) {
            throw new Error("the server is not running");
        }
        return this.connected;
    }

    /// Another reason the run failed, after those before it: a request that
    /// timed out under a server that then turns out to have crashed is told
    /// as both.
    fail(error: unknown): void {
        const message = error instanceof Error ? error.message : String(error);
        this.result.error =
            this.result.error === undefined ? message : `${this.result.error}; ${message}`;
    }

    record(name: string, ms: number): void {
        (this.result.series[name] ??= []).push(Math.round(ms * 10) / 10);
    }

    async measure(name: string, ask: () => Promise<unknown>): Promise<unknown> {
        const start = nowMs();
        const answer = await bounded(ask(), name);
        this.record(name, nowMs() - start);
        return answer;
    }

    /// The server started as an editor starts it, behind `wrap` when given
    /// (a command that runs the rest, as a memory limit does).
    async start(wrap: readonly string[] = []): Promise<CliceClient> {
        const { workspace, database, binaries, clangdArgs } = this.context;
        const binary = binaries[this.server];
        if (binary === undefined) {
            throw new Error(`no ${this.server} to start`);
        }
        if (isClice(this.server)) {
            this.priorLogs = new Set(logFiles(workspace));
        }
        const args = isClice(this.server)
            ? ["serve"]
            : [
                  `--compile-commands-dir=${database}`,
                  // The counterpart of clice's rule overlay: no .clangd or
                  // user config edits the commands either.
                  "--enable-config=0",
                  ...clangdArgs,
              ];
        const command = wrap[0] ?? binary;
        const prefix = wrap.length === 0 ? [] : [...wrap.slice(1), binary];
        this.started = nowMs();
        const client = CliceClient.start(command, {
            args: [...prefix, ...args],
            // A relative database `directory` anchors at the database for
            // clice but at the process's cwd for clangd; clangd runs from the
            // database's directory so that both compile under one command.
            cwd: isClice(this.server) ? undefined : database,
            checked: false,
        });
        this.connected = client;
        // A wrapper this process may not signal (sudo's memory scope) fails a
        // kill with an error event: the server's exit tells how it ended.
        client.child.on("error", () => undefined);
        if (client.child.pid !== undefined) {
            this.watch = watchTree(client.child.pid, isClice(this.server) ? "master" : "clangd");
        }
        const initialized = client.initialize(new Workspace(workspace), {
            initializationOptions: {
                // clice reads the database the job chose, as clangd is told
                // to: the overlay replaces the rules of any clice.toml, so
                // none of its flag edits apply either.
                rules: [{ compile_commands: [database] }],
                project: {
                    // Logs where finish() reads them (logFiles searches
                    // <workspace>/.clice/logs).
                    logging_dir: path.join(workspace, ".clice", "logs"),
                },
            },
            cacheDir: cliceDir(workspace, this.server),
            testDefaults: false,
            capabilities: CAPABILITIES,
        });
        await bounded(initialized, "initialize");
        this.record("initialize", nowMs() - this.started);
        return client;
    }

    /// Opens a file as an editor does, asking its semantic tokens at once,
    /// and waits for its first diagnostics and the tokens: the time to the
    /// diagnostics.
    async open(file: string): Promise<{ uri: string; ms: number }> {
        const client = this.client;
        const opened = nowMs();
        const [uri, text] = client.open(path.resolve(this.context.workspace, file));
        const lines = text.split("\n");
        this.versions.set(uri, 0);
        this.ends.set(uri, { line: lines.length - 1, character: lines.at(-1)?.length ?? 0 });
        const diagnostics = bounded(client.armDiagnostics(uri), `diagnostics of ${file}`).then(
            () => nowMs() - opened,
        );
        await bounded(client.semanticTokensFull(uri), `semantic tokens of ${file}`);
        return { uri, ms: await diagnostics };
    }

    /// Where an edit at the end of the file goes.
    end(uri: string): proto.Position {
        return this.ends.get(uri) ?? { line: 0, character: 0 };
    }

    /// Appends a line at the end of the file: every edit invalidates the
    /// opened file but not its preamble, the interactive path. Sent as a
    /// ranged insertion like a real editor, followed by the semantic tokens
    /// an editor asks after every change (they pull clice's rebuild too):
    /// the times to the new diagnostics and to the tokens.
    async edit(uri: string): Promise<{ diagnostics: number; tokens: number }> {
        const client = this.client;
        const version = (this.versions.get(uri) ?? 0) + 1;
        this.versions.set(uri, version);
        const end = this.end(uri);
        const comment = `// perf edit ${String(version)}`;
        const edited = nowMs();
        const arrived = client.armDiagnostics(uri);
        client.changeRange(uri, version, { start: end, end }, `\n${comment}`);
        const [diagnostics, tokens] = await Promise.all([
            bounded(arrived, `diagnostics of ${uri}`).then(() => nowMs() - edited),
            bounded(client.semanticTokensFull(uri), `semantic tokens of ${uri}`).then(
                () => nowMs() - edited,
            ),
        ]);
        this.ends.set(uri, { line: end.line + 1, character: comment.length });
        return { diagnostics, tokens };
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

    /// Waits until the server is idle: no background index round running,
    /// none begun and the tree at a tenth of a core at most for QUIET_MS,
    /// at most `limit` ms. With `round`, a round must have ended first.
    /// Returns when the last round ended (or when it went quiet), the most
    /// files a round counted, and whether it went idle at all.
    async idle(
        round: boolean,
        limit = INDEX_BOUND,
    ): Promise<{ ended: number; files: number; idle: boolean }> {
        const client = this.client;
        const token = indexToken(this.server);
        const began = nowMs();
        const bound = Math.min(limit, this.context.deadline - Date.now());
        let seen = 0;
        let changed = nowMs();
        let active = false;
        let ended: number | null = null;
        let announced = false;
        let files = 0;
        while (nowMs() - began < bound) {
            if (client.child.exitCode !== null || client.child.signalCode !== null) {
                throw new Error(`${this.server} exited while it was awaited to go idle`);
            }
            const events = client.progressEvents;
            for (; seen < events.length; seen += 1) {
                const event = events[seen];
                if (event?.token !== token) {
                    continue;
                }
                const value = event.value as { kind?: string; message?: string };
                announced = true;
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
            if (!active && (!round || ended !== null) && nowMs() - changed >= QUIET_MS) {
                if (this.busy() < 0.1) {
                    return { ended: ended ?? changed, files, idle: true };
                }
            }
            if (round && !announced && nowMs() - this.started > ANNOUNCE_BOUND) {
                throw new Error(`${this.server} announced no background index round`);
            }
            await pause(1000);
        }
        return { ended: ended ?? nowMs(), files, idle: false };
    }

    /// The cores the tree kept busy over the last QUIET_MS.
    private busy(): number {
        const timeline = this.watch?.timeline() ?? [];
        const last = timeline.at(-1);
        const first = timeline.find((sample) => (last?.t ?? 0) - sample.t <= QUIET_MS);
        if (last === undefined || first === undefined || last.t - first.t < QUIET_MS / 2) {
            return 1;
        }
        return (last.cpu - first.cpu) / ((last.t - first.t) / 1000);
    }

    /// The tree's CPU seconds so far.
    cpu(): number {
        return this.watch?.latest()?.cpu ?? 0;
    }

    /// What the tree holds now, under `name`.
    snapshot(name: string): void {
        const latest = this.watch?.latest();
        if (latest !== undefined) {
            this.result.snapshots[name] = {
                t: Math.round((nowMs() - this.started) / 100) / 10,
                total: latest.total,
                roles: latest.roles,
                cpu: latest.cpu,
            };
        }
    }

    /// The tree's memory over the run, clice's perf lines, and the server
    /// shut down by the protocol, judged by its exit code alone; once.
    async finish(): Promise<RunResult> {
        const client = this.connected;
        if (client === null) {
            return this.result;
        }
        this.connected = null;
        try {
            const watched = this.watch?.stop();
            if (watched !== undefined) {
                const { timeline, ...memory } = watched;
                this.result.memory = memory;
                this.result.timeline = timeline;
            }
            if (isClice(this.server)) {
                this.collectPerf(client);
            }
        } finally {
            await shutdown(client, this.server);
        }
        return this.result;
    }

    /// Server-side breakdown from the session's log files — the worker
    /// processes' `[perf:build]`/`[perf:query]` lines only exist there
    /// (workers never mirror to stderr). Captured stderr, which carries the
    /// master's lines, is the fallback when no log files appeared. A worker
    /// that crashed, or an anomaly, fails the run even when the master
    /// recovered.
    private collectPerf(client: CliceClient): void {
        const fresh = logFiles(this.context.workspace).filter((f) => !this.priorLogs.has(f));
        const texts =
            fresh.length > 0
                ? fresh.map((f) => fs.readFileSync(f, "utf8"))
                : [client.drainedStderr().toString("utf8")];
        const failed = texts
            .flatMap((text) => text.split(/\r?\n/))
            .find((line) => CRASHED_IN.test(line) || ANOMALY_PATTERN.test(line));
        if (failed !== undefined) {
            this.fail(`clice logged: ${failed}`);
        }
        // One pid per log file: master and workers are separate processes
        // and keep separate trace lanes.
        let events = texts.flatMap((text, i) => parsePerfLines(text, i + 1));
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

/// The server killed, and on Linux its wrapper's children too: a server
/// behind a wrapper this process may not signal (sudo's memory scope) runs
/// as this process's user itself.
function killHard(client: CliceClient): void {
    client.child.kill("SIGKILL");
    const { pid } = client.child;
    if (process.platform !== "linux" || pid === undefined) {
        return;
    }
    try {
        const children = fs.readFileSync(
            `/proc/${String(pid)}/task/${String(pid)}/children`,
            "utf8",
        );
        for (const child of children.trim().split(/\s+/).filter(Boolean)) {
            process.kill(Number(child), "SIGKILL");
        }
    } catch {
        // Gone already.
    }
}

/// Both servers leave by the protocol, judged by their exit code alone:
/// the test harness's clean-exit gate scans stderr for sanitizer names,
/// which a release build's log carries in LLVM's file names
/// (AddressSanitizer.cpp).
async function shutdown(client: CliceClient, server: ServerName): Promise<void> {
    try {
        await withTimeout(client.sendRequest(proto.ShutdownRequest.type), 60_000, "shutdown");
    } finally {
        // A server that failed its shutdown is told to exit all the same; a
        // dead one has no connection left to take the notification.
        await client.sendNotification(proto.ExitNotification.type).catch(() => undefined);
        client.disposed = true;
        // Hang detection: a server still running a minute after `exit` is
        // killed.
        setTimeout(() => {
            killHard(client);
        }, 60_000).unref();
        await client.exited;
    }
    const { exitCode, signalCode } = client.child;
    if (exitCode !== 0) {
        throw new Error(`${server} exited with ${signalCode ?? `code ${String(exitCode)}`}`);
    }
}
