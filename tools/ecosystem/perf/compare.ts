/// Tables from perf results: for every scenario and measure, the median
/// and p90 of each server over its runs, and the ratio of clice's median to
/// clangd's (and to the baseline's, when one ran). A scenario run over many
/// files pools them, its ratio the median of the files' ratios. Results pool
/// only within one job on one machine: another job or machine gets a table
/// of its own.
///
/// Usage:
///   node tools/ecosystem/perf/compare.ts <results.json>...
///
/// Times are milliseconds, memory and disk megabytes; `size` is what the
/// last answer held (items, locations, tokens), so a time is read beside
/// the work it bought.

import * as fs from "node:fs";
import { computeStats } from "./logs.ts";
import type { PerfResults, RunResult } from "./results.ts";

/// A column: a server, and what sets its runs apart.
function columnOf(run: RunResult): string {
    return run.variant === undefined ? run.server : `${run.server} (${run.variant})`;
}

/// Every measure a run took, each with its samples.
function measures(run: RunResult): [string, number[]][] {
    const out: [string, number[]][] = Object.entries(run.series);
    for (const [name, size] of Object.entries(run.sizes)) {
        out.push([`${name} size`, [size]]);
    }
    for (const [name, value] of Object.entries(run.values)) {
        out.push([name, [value]]);
    }
    if (run.memory !== undefined) {
        out.push(["peak memory MB", [run.memory.peak.total]]);
        out.push(["end memory MB", [run.memory.end.root + run.memory.end.children]]);
        out.push(["CPU s", [run.memory.cpu]]);
    }
    for (const [name, snapshot] of Object.entries(run.snapshots)) {
        out.push([`memory ${name} MB`, [snapshot.total]]);
    }
    return out;
}

function cell(values: readonly number[] | undefined, at: "p50" | "p90"): string {
    const stats = values === undefined ? null : computeStats([...values]);
    if (stats === null) {
        return "–";
    }
    const value = stats[at];
    return value >= 100 ? value.toFixed(0) : value.toFixed(1);
}

function medianOf(values: readonly number[]): number | undefined {
    return computeStats([...values])?.p50;
}

/// The ratio of `of`'s median to `to`'s, a median of the files' ratios
/// where the scenario ran over several.
function ratio(byFile: Map<string, Map<string, number[]>>, of: string, to: string): string {
    const ratios = [...byFile.values()].flatMap((columns) => {
        const a = medianOf(columns.get(of) ?? []);
        const b = medianOf(columns.get(to) ?? []);
        return a === undefined || b === undefined || b === 0 ? [] : [a / b];
    });
    const pooled = medianOf(ratios);
    return pooled === undefined ? "–" : `${pooled.toFixed(2)}×`;
}

export function tables(results: readonly PerfResults[]): string[] {
    const groups = new Map<string, { result: PerfResults; runs: RunResult[] }>();
    for (const result of results) {
        const machine = [result.env.platform, result.env.cpuModel, result.env.cpus];
        const key = JSON.stringify([result.job, result.clangdArgs, machine]);
        const group = groups.get(key) ?? { result, runs: [] };
        group.runs.push(...result.runs);
        groups.set(key, group);
    }
    const lines: string[] = [];
    for (const { result, runs } of groups.values()) {
        const { env } = result;
        lines.push(
            `### ${result.job}`,
            "",
            `${env.platform} ${env.arch}, ${env.cpuModel}, ${String(env.cpus)} CPUs, ${String(env.memoryMB)} MB; ` +
                Object.entries(result.binaries)
                    .map(([server, binary]) => `${server} ${binary}`)
                    .join(", "),
            "",
        );
        const columns = [...new Set(runs.map(columnOf))];
        // A clice variant compares with the same variant of the other server
        // where it ran one, else with its usual run.
        const ratios = columns
            .filter((column) => column.startsWith("clice"))
            .flatMap((of) =>
                (["clangd", "baseline"] as const).flatMap((server) => {
                    const same = of.replace(/^clice/, server);
                    const to = columns.includes(same) ? same : server;
                    return columns.includes(to) ? [{ of, to }] : [];
                }),
            );
        lines.push(
            `| scenario | measure | ${columns.map((c) => `${c} p50 | ${c} p90`).join(" | ")} |${ratios.map(({ of, to }) => ` ${of}/${to} |`).join("")}`,
            `|---|---|${columns.map(() => "---:|---:").join("|")}|${ratios.map(() => "---:|").join("")}`,
        );
        const scenarios = [...new Set(runs.map((run) => `${run.scenario} ${run.label}`))];
        for (const name of scenarios) {
            const ofScenario = runs.filter((run) => `${run.scenario} ${run.label}` === name);
            // A failed run counts here, not in the medians: a crashed server
            // reads as holding no memory.
            const failed = columns.map(
                (column) =>
                    ofScenario.filter((run) => columnOf(run) === column && run.error !== undefined)
                        .length,
            );
            if (failed.some((count) => count > 0)) {
                lines.push(
                    `| ${name} | failed runs | ${failed.map((count) => `${String(count)} | –`).join(" | ")} |${ratios.map(() => " – |").join("")}`,
                );
            }
            // measure → file → column → samples
            const table = new Map<string, Map<string, Map<string, number[]>>>();
            for (const run of ofScenario.filter((each) => each.error === undefined)) {
                for (const [measure, values] of measures(run)) {
                    const byFile = table.get(measure) ?? new Map<string, Map<string, number[]>>();
                    const file = run.file ?? run.label;
                    const byColumn = byFile.get(file) ?? new Map<string, number[]>();
                    byColumn.set(columnOf(run), [
                        ...(byColumn.get(columnOf(run)) ?? []),
                        ...values,
                    ]);
                    byFile.set(file, byColumn);
                    table.set(measure, byFile);
                }
            }
            for (const [measure, byFile] of table) {
                const pooled = (column: string) =>
                    [...byFile.values()].flatMap((byColumn) => byColumn.get(column) ?? []);
                const cells = columns.map(
                    (column) => `${cell(pooled(column), "p50")} | ${cell(pooled(column), "p90")}`,
                );
                lines.push(
                    `| ${name} | ${measure} | ${cells.join(" | ")} |${ratios.map(({ of, to }) => ` ${ratio(byFile, of, to)} |`).join("")}`,
                );
            }
        }
        lines.push("");
    }
    return lines;
}

function main(): void {
    const files = process.argv.slice(2);
    if (files.length === 0) {
        console.error("usage: node tools/ecosystem/perf/compare.ts <results.json>...");
        process.exit(1);
    }
    const results = files.map((file) => JSON.parse(fs.readFileSync(file, "utf8")) as PerfResults);
    console.log(tables(results).join("\n"));
}

if (import.meta.main) {
    main();
}
