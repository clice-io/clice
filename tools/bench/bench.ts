/// E2E benchmark harness: drive a language server over LSP through fixed
/// scenarios on a real workspace and report client-observed latencies.
///
/// The harness is server-agnostic at the LSP level, so the same scenarios
/// run against clice and clangd for A/B comparison. For clice it
/// additionally parses the `[perf:*]` lines mirrored to stderr into a
/// server-side breakdown (see perf.ts).
///
/// Usage:
///   node tools/bench/bench.ts --workspace <dir> [options]
///
/// Options:
///   --server clice|clangd    which server to drive (default clice)
///   --binary <path>          server executable (default: clice from
///                            build/RelWithDebInfo/bin, clangd from PATH)
///   --file <rel>             file to open/edit (default: first CDB entry)
///   --position <line:char>   position for warm requests (default 0:0)
///   --scenario <name>        cold_start | warm_start | edit_loop |
///                            warm_requests; repeatable (default: all)
///   --edits <N>              edit_loop iterations (default 10)
///   --repeats <N>            warm request repetitions (default 50)
///   --json <path>            write the full results as JSON
///
/// The workspace must contain a compile_commands.json. cold_start wipes
/// the workspace's .clice cache dir (and clangd's .cache) first; the other
/// scenarios reuse whatever state the previous scenarios left, in order.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { parseArgs } from "node:util";
import { fileURLToPath } from "node:url";
import { CliceClient } from "../client/client.ts";
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
    position: { line: number; character: number };
    scenarios: ScenarioName[];
    edits: number;
    repeats: number;
    jsonPath: string | null;
}

interface ScenarioResult {
    /// Client-observed latency series, milliseconds.
    measurements: Record<string, Stats>;
    /// Server-side breakdown from `[perf:*]` stderr lines (clice only).
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
            position: { type: "string", default: "0:0" },
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

    const [lineText, charText] = values.position.split(":");
    const line = Number(lineText);
    const character = Number(charText);
    if (!Number.isInteger(line) || !Number.isInteger(character)) {
        fail(`bad --position '${values.position}' (expected line:char)`);
    }

    return {
        server,
        binary,
        workspace: path.resolve(values.workspace),
        cdbDir: "",
        file: values.file ?? null,
        position: { line, character },
        scenarios,
        edits: Number(values.edits),
        repeats: Number(values.repeats),
        jsonPath: values.json ?? null,
    };
}

/// Locate the CDB the way clice does: workspace root first, then any
/// first-level subdirectory (e.g. build/).
function findCdb(workspace: string): string {
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

function firstCdbEntry(cdbPath: string): string {
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

    record(name: string, ms: number): void {
        const list = this.series.get(name) ?? [];
        list.push(ms);
        this.series.set(name, list);
    }

    async measure(name: string, fn: () => Promise<unknown>): Promise<void> {
        this.record(name, await timed(fn));
    }

    result(client: CliceClient, server: string): ScenarioResult {
        const measurements: Record<string, Stats> = {};
        for (const [name, values] of this.series) {
            const stats = computeStats(values);
            if (stats !== null) {
                measurements[name] = stats;
            }
        }
        const result: ScenarioResult = { measurements };
        if (server === "clice") {
            const perf = summarize(parsePerfLines(client.drainedStderr().toString("utf8")));
            if (Object.keys(perf).length > 0) {
                result.perf = perf;
            }
        }
        return result;
    }
}

/// clice runs `serve` and discovers the CDB itself; clangd needs the CDB
/// directory spelled out.
function serverArgs(opts: Options): string[] {
    return opts.server === "clice"
        ? ["serve"]
        : ["--background-index", `--compile-commands-dir=${opts.cdbDir}`];
}

/// CliceClient.initialize defaults worker counts to 1 for cheap tests; a
/// benchmark must run the server's real defaults (stateful 2, stateless
/// cores/2, from src/server/state/config.cpp).
function initializationOptions(): Record<string, unknown> {
    return {
        project: {
            stateful_worker_count: 2,
            stateless_worker_count: Math.max(Math.floor(os.cpus().length / 2), 2),
        },
    };
}

async function startServer(opts: Options): Promise<CliceClient> {
    const client = CliceClient.start(opts.binary, { args: serverArgs(opts) });
    await client.initialize(new Workspace(opts.workspace), {
        initializationOptions: initializationOptions(),
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
async function runStartScenario(opts: Options, file: string): Promise<ScenarioResult> {
    const bench = new Bench();

    const start = nowMs();
    const client = CliceClient.start(opts.binary, { args: serverArgs(opts) });
    await client.initialize(new Workspace(opts.workspace), {
        initializationOptions: initializationOptions(),
    });
    bench.record("initialize", nowMs() - start);

    await bench.measure("open_to_diagnostics", () => client.openAndWait(file, 300_000));

    const result = bench.result(client, opts.server);
    await shutdownServer(client, opts);
    return result;
}

async function runColdStart(opts: Options, file: string): Promise<ScenarioResult> {
    // clice caches in the workspace's .clice (pinned by the client's
    // initialize); clangd keeps its index under .cache next to the
    // workspace root and the CDB directory.
    for (const dir of [".clice", ".cache"]) {
        fs.rmSync(path.join(opts.workspace, dir), { recursive: true, force: true });
        fs.rmSync(path.join(opts.cdbDir, dir), { recursive: true, force: true });
    }
    return runStartScenario(opts, file);
}

async function runEditLoop(opts: Options, file: string): Promise<ScenarioResult> {
    const bench = new Bench();
    const client = await startServer(opts);
    const [uri, content] = await client.openAndWait(file, 300_000);

    for (let i = 1; i <= opts.edits; i += 1) {
        // Append at the end of the TU: every edit invalidates the main file
        // but not the preamble — the interactive path the server optimizes.
        const edited = `${content}\n// bench edit ${i}\n`;
        await bench.measure("edit_to_diagnostics", async () => {
            client.change(uri, i, edited);
            await client.waitForRecompile(uri, 300_000);
        });
    }

    const result = bench.result(client, opts.server);
    await shutdownServer(client, opts);
    return result;
}

async function runWarmRequests(opts: Options, file: string): Promise<ScenarioResult> {
    const bench = new Bench();
    const client = await startServer(opts);
    const [uri] = await client.openAndWait(file, 300_000);
    const { line, character } = opts.position;

    const requests: Record<string, () => Promise<unknown>> = {
        hover: () => client.hoverAt(uri, line, character),
        definition: () => client.definitionAt(uri, line, character),
        references: () => client.referencesAt(uri, line, character),
        completion: () => client.completionAt(uri, line, character),
        document_symbol: () => client.documentSymbols(uri),
        semantic_tokens: () => client.semanticTokensFull(uri),
    };

    for (const [name, request] of Object.entries(requests)) {
        // One unmeasured warmup absorbs lazy first-request work.
        await request();
        for (let i = 0; i < opts.repeats; i += 1) {
            await bench.measure(name, request);
        }
    }

    const result = bench.result(client, opts.server);
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
    const cdb = findCdb(opts.workspace);
    opts.cdbDir = path.dirname(cdb);
    const file = opts.file !== null ? path.resolve(opts.workspace, opts.file) : firstCdbEntry(cdb);

    const result: BenchResult = {
        env: {
            platform: process.platform,
            release: os.release(),
            arch: process.arch,
            cpus: os.cpus().length,
            cpuModel: os.cpus()[0]?.model ?? "unknown",
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
