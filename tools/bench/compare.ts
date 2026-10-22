/// Tables from bench.ts results: for every scenario and measure, the median
/// and p90 of each server over all runs of all files given, and the ratio
/// of clice's median to clangd's. Files of different workspaces or focus
/// files get a table each.
///
/// Usage:
///   node tools/bench/compare.ts <results.json>...
///
/// Times are milliseconds, memory and disk megabytes; `size` is what the
/// last answer held (items, locations, tokens), so a time is read beside
/// the work it bought.

import * as fs from "node:fs";
import * as path from "node:path";
import {
    SCENARIOS,
    SERVERS,
    type BenchResult,
    type RunResult,
    type ServerName,
} from "./results.ts";
import { computeStats } from "./perf.ts";

const files = process.argv.slice(2);
if (files.length === 0) {
    console.error("usage: node tools/bench/compare.ts <results.json>...");
    process.exit(1);
}

/// Every sample of one measure, per server.
type Samples = Partial<Record<ServerName, number[]>>;

const groups = new Map<string, { result: BenchResult; runs: RunResult[] }>();
for (const file of files) {
    const result = JSON.parse(fs.readFileSync(file, "utf8")) as BenchResult;
    const key = `${result.workspace} ${result.file}`;
    const group = groups.get(key) ?? { result, runs: [] };
    group.runs.push(...result.runs);
    groups.set(key, group);
}

function add(
    table: Map<string, Samples>,
    measure: string,
    server: ServerName,
    value: number,
): void {
    const samples = table.get(measure) ?? {};
    (samples[server] ??= []).push(value);
    table.set(measure, samples);
}

function cell(values: number[] | undefined, at: "p50" | "p90"): string {
    const stats = values === undefined ? null : computeStats(values);
    if (stats === null) {
        return "–";
    }
    const value = stats[at];
    return value >= 100 ? value.toFixed(0) : value.toFixed(1);
}

function ratio(samples: Samples): string {
    const clice = computeStats(samples.clice ?? []);
    const clangd = computeStats(samples.clangd ?? []);
    return clice === null || clangd === null || clangd.p50 === 0
        ? "–"
        : `${(clice.p50 / clangd.p50).toFixed(2)}×`;
}

for (const { result, runs } of groups.values()) {
    const { env } = result;
    // Results of a Windows run read the same on any machine.
    const paths = env.platform === "win32" ? path.win32 : path.posix;
    console.log(
        `\n### ${paths.basename(result.workspace)}: ${paths.relative(result.workspace, result.file)}\n`,
    );
    console.log(
        `${env.platform} ${env.arch}, ${env.cpuModel}, ${env.cpus} CPUs, ${env.memoryMB} MB; ` +
            `probes ${JSON.stringify(result.probes)}, query ${result.query}\n`,
    );
    const servers = SERVERS.filter((server) => runs.some((run) => run.server === server));
    console.log(
        `| scenario | measure | ${servers.map((s) => `${s} p50 | ${s} p90`).join(" | ")} | clice/clangd |`,
    );
    console.log(`|---|---|${servers.map(() => "---:|---:").join("|")}|---:|`);
    for (const scenario of SCENARIOS) {
        const table = new Map<string, Samples>();
        // A failed run counts below, not in the medians: a crashed server
        // reads as holding no memory.
        for (const run of runs.filter(
            (each) => each.scenario === scenario && each.error === undefined,
        )) {
            for (const [measure, values] of Object.entries(run.series)) {
                for (const value of values) {
                    add(table, measure, run.server, value);
                }
            }
            for (const [measure, size] of Object.entries(run.sizes)) {
                add(table, `${measure} size`, run.server, size);
            }
            if (run.memory !== undefined) {
                add(table, "peak memory MB", run.server, run.memory.peak.total);
                add(
                    table,
                    "end memory MB",
                    run.server,
                    run.memory.end.root + run.memory.end.children,
                );
            }
            if (run.index !== undefined) {
                add(table, "indexed files", run.server, run.index.files);
                add(table, "index MB on disk", run.server, run.index.indexMB);
                add(table, "cache MB on disk", run.server, run.index.diskMB);
            }
            if (run.dropped !== undefined) {
                add(table, "file cache dropped", run.server, run.dropped ? 1 : 0);
            }
        }
        const failed = servers.map(
            (server) =>
                runs.filter(
                    (run) =>
                        run.scenario === scenario &&
                        run.server === server &&
                        run.error !== undefined,
                ).length,
        );
        if (failed.some((count) => count > 0)) {
            console.log(
                `| ${scenario} | failed runs | ${failed.map((count) => `${count} | – | `).join("")}– |`,
            );
        }
        for (const [measure, samples] of table) {
            const cells = servers.map(
                (server) => `${cell(samples[server], "p50")} | ${cell(samples[server], "p90")}`,
            );
            console.log(`| ${scenario} | ${measure} | ${cells.join(" | ")} | ${ratio(samples)} |`);
        }
    }
}
