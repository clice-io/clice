/// Tables from perf results: for every scenario and measure, the median
/// and p90 of each server over its runs, and the ratio of clice's median to
/// clangd's and the baseline's, each that ran. A scenario run over many
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
import type { PerfResults, RunResult, Slower } from "./results.ts";

/// A column: a server, and what sets its runs apart.
function columnOf(run: RunResult): string {
    return run.variant === undefined ? run.server : `${run.server} (${run.variant})`;
}

/// Every measure a run took, each with its samples, and whether more of it
/// costs more: a time, memory, disk or CPU, not a count or what an answer
/// held.
function measures(run: RunResult): { name: string; values: number[]; cost: boolean }[] {
    const out = Object.entries(run.series).map(([name, values]) => ({ name, values, cost: true }));
    for (const [name, size] of Object.entries(run.sizes)) {
        out.push({ name: `${name} size`, values: [size], cost: false });
    }
    for (const [name, value] of Object.entries(run.values)) {
        out.push({ name, values: [value], cost: / (ms|s|MB)( |$)/.test(name) });
    }
    if (run.memory !== undefined) {
        out.push({ name: "peak memory MB", values: [run.memory.peak.total], cost: true });
        out.push({
            name: "end memory MB",
            values: [run.memory.end.root + run.memory.end.children],
            cost: true,
        });
        out.push({ name: "CPU s", values: [run.memory.cpu], cost: true });
    }
    for (const [name, snapshot] of Object.entries(run.snapshots)) {
        out.push({ name: `memory ${name} MB`, values: [snapshot.total], cost: true });
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

/// file → column → samples
type ByFile = Map<string, Map<string, number[]>>;

/// The ratio of `of`'s median to `to`'s, a median of the files' ratios
/// where the scenario ran over several.
function ratioOf(byFile: ByFile, of: string, to: string): number | undefined {
    const ratios = [...byFile.values()].flatMap((columns) => {
        const a = medianOf(columns.get(of) ?? []);
        const b = medianOf(columns.get(to) ?? []);
        return a === undefined || b === undefined || b === 0 ? [] : [a / b];
    });
    return medianOf(ratios);
}

function pooled(byFile: ByFile, column: string): number[] {
    return [...byFile.values()].flatMap((byColumn) => byColumn.get(column) ?? []);
}

/// One job's runs on one machine: its columns, the pairs of them a ratio
/// compares, and each scenario's measures.
interface Group {
    result: PerfResults;
    columns: string[];
    ratios: { of: string; to: string }[];
    scenarios: {
        scenario: string;
        name: string;
        /// Per column, the runs that failed.
        failed: number[];
        measures: Map<string, { cost: boolean; byFile: ByFile }>;
    }[];
}

function groupsOf(results: readonly PerfResults[]): Group[] {
    const byMachine = new Map<string, { result: PerfResults; runs: RunResult[] }>();
    for (const result of results) {
        const machine = [result.env.platform, result.env.cpuModel, result.env.cpus];
        const key = JSON.stringify([result.job, result.clangdArgs, machine]);
        const group = byMachine.get(key) ?? { result, runs: [] };
        group.runs.push(...result.runs);
        byMachine.set(key, group);
    }
    return [...byMachine.values()].map(({ result, runs }) => {
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
        const names = [...new Set(runs.map((run) => `${run.scenario} ${run.label}`))];
        const scenarios = names.map((name) => {
            const ofScenario = runs.filter((run) => `${run.scenario} ${run.label}` === name);
            // A failed run counts here, not in the medians: a crashed server
            // reads as holding no memory.
            const failed = columns.map(
                (column) =>
                    ofScenario.filter((run) => columnOf(run) === column && run.error !== undefined)
                        .length,
            );
            const measured = new Map<string, { cost: boolean; byFile: ByFile }>();
            for (const run of ofScenario.filter((each) => each.error === undefined)) {
                for (const { name: measure, values, cost } of measures(run)) {
                    const entry = measured.get(measure) ?? {
                        cost,
                        byFile: new Map<string, Map<string, number[]>>(),
                    };
                    const file = run.file ?? run.label;
                    const byColumn = entry.byFile.get(file) ?? new Map<string, number[]>();
                    byColumn.set(columnOf(run), [
                        ...(byColumn.get(columnOf(run)) ?? []),
                        ...values,
                    ]);
                    entry.byFile.set(file, byColumn);
                    measured.set(measure, entry);
                }
            }
            return { scenario: ofScenario[0]?.scenario ?? "", name, failed, measures: measured };
        });
        return { result, columns, ratios, scenarios };
    });
}

export function tables(results: readonly PerfResults[]): string[] {
    const lines: string[] = [];
    for (const { result, columns, ratios, scenarios } of groupsOf(results)) {
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
        lines.push(
            `| scenario | measure | ${columns.map((c) => `${c} p50 | ${c} p90`).join(" | ")} |${ratios.map(({ of, to }) => ` ${of}/${to} |`).join("")}`,
            `|---|---|${columns.map(() => "---:|---:").join("|")}|${ratios.map(() => "---:|").join("")}`,
        );
        for (const { name, failed, measures: measured } of scenarios) {
            if (failed.some((count) => count > 0)) {
                lines.push(
                    `| ${name} | failed runs | ${failed.map((count) => `${String(count)} | –`).join(" | ")} |${ratios.map(() => " – |").join("")}`,
                );
            }
            for (const [measure, { byFile }] of measured) {
                const cells = columns.map(
                    (column) =>
                        `${cell(pooled(byFile, column), "p50")} | ${cell(pooled(byFile, column), "p90")}`,
                );
                const shown = ratios.map(({ of, to }) => {
                    const value = ratioOf(byFile, of, to);
                    return ` ${value === undefined ? "–" : `${value.toFixed(2)}×`} |`;
                });
                lines.push(`| ${name} | ${measure} | ${cells.join(" | ")} |${shown.join("")}`);
            }
        }
        lines.push("");
    }
    return lines;
}

/// How much more a measure may cost clice than the baseline before the
/// summary lists it.
const SLOWER = 1.1;
/// Under this many of its unit (milliseconds, seconds, megabytes) on the
/// baseline, a measure's noise between two runs is more than a tenth.
const NOISE_FLOOR = 10;

/// The measures of a job's runs where clice costs at least a tenth more
/// than the baseline, each clice column against its baseline one.
export function slower(result: PerfResults): Omit<Slower, "repro">[] {
    const out: Omit<Slower, "repro">[] = [];
    for (const { ratios, scenarios } of groupsOf([result])) {
        const pairs = ratios.filter(({ to }) => to.startsWith("baseline"));
        for (const { scenario, name, measures: measured } of scenarios) {
            for (const [measure, { cost, byFile }] of measured) {
                for (const { of, to } of pairs) {
                    const base = medianOf(pooled(byFile, to));
                    const ratio = ratioOf(byFile, of, to);
                    if (
                        !cost ||
                        base === undefined ||
                        base < NOISE_FLOOR ||
                        ratio === undefined ||
                        ratio < SLOWER
                    ) {
                        continue;
                    }
                    out.push({
                        scenario,
                        name,
                        measure:
                            of === "clice" ? measure : `${measure} ${of.slice("clice ".length)}`,
                        clice: cell(pooled(byFile, of), "p50"),
                        baseline: cell(pooled(byFile, to), "p50"),
                        clicePerBaseline: ratio,
                    });
                }
            }
        }
    }
    return out;
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
