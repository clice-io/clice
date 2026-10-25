/// Tests for the perf tables: medians per server, ratios of the medians,
/// a scenario over many files pooled by its files' ratios, a variant
/// compared with the same variant of the other server, and what clice
/// spends more of than a baseline.

import { expect, test } from "vitest";
import { perfReport } from "../report.ts";
import { slower, tables } from "./compare.ts";
import { failureOf, type PerfOptions } from "./perf.ts";
import type { PerfResults, RunResult } from "./results.ts";

function run(
    server: RunResult["server"],
    scenario: string,
    series: Record<string, number[]>,
    extra: Partial<RunResult> = {},
): RunResult {
    return {
        server,
        scenario,
        label: "a.cpp",
        run: 1,
        series,
        sizes: {},
        values: {},
        snapshots: {},
        ...extra,
    };
}

function results(runs: RunResult[]): PerfResults {
    return {
        job: "p/editing",
        platform: "linux",
        about: "",
        commit: "c",
        clice: "k",
        env: {
            platform: "linux",
            release: "",
            arch: "x64",
            cpus: 4,
            cpuModel: "cpu",
            memoryMB: 16000,
            node: "",
            date: "",
        },
        binaries: { clice: "/c", clangd: "/d" },
        clangdArgs: [],
        runs,
        failures: [],
        known: [],
        unseen: [],
        slower: [],
    };
}

test("a measure's medians and p90s per server, and the ratio of the medians", () => {
    const lines = tables([
        results([
            run("clice", "editLoop", { edit: [10, 20, 30] }),
            run("clangd", "editLoop", { edit: [40, 40, 40] }),
            run("clice", "editLoop", { edit: [] }, { error: "clice exited with SIGSEGV" }),
        ]),
    ]);
    expect(lines).toContain(
        "| scenario | measure | clice p50 | clice p90 | clangd p50 | clangd p90 | clice/clangd |",
    );
    expect(lines).toContain("| editLoop a.cpp | failed runs | 1 | – | 0 | – | – |");
    expect(lines).toContain("| editLoop a.cpp | edit | 20.0 | 20.0 | 40.0 | 40.0 | 0.50× |");
});

test("a scenario over many files takes the median of its files' ratios", () => {
    const sample = (server: RunResult["server"], file: string, open: number, variant?: string) =>
        run(
            server,
            "sample",
            { open: [open] },
            {
                label: "3 files",
                file,
                ...(variant === undefined ? {} : { variant }),
            },
        );
    const lines = tables([
        results([
            sample("clangd", "a", 100),
            sample("clangd", "b", 10),
            sample("clangd", "c", 1000),
            sample("clice", "a", 200),
            sample("clice", "b", 10),
            sample("clice", "c", 3000),
            sample("clice", "a", 400, "no PCH"),
            sample("clice", "b", 40, "no PCH"),
            sample("clice", "c", 4000, "no PCH"),
        ]),
    ]);
    expect(lines).toContain(
        "| sample 3 files | open | 100 | 100 | 200 | 200 | 400 | 400 | 2.00× | 4.00× |",
    );
});

test("a variant compares with the other server's same variant", () => {
    const start = (server: RunResult["server"], ms: number, variant?: string) =>
        run(server, "sample", { open: [ms] }, variant === undefined ? {} : { variant });
    const lines = tables([
        results([
            start("clangd", 50, "no cache"),
            start("clangd", 10),
            start("clice", 100, "no cache"),
            start("clice", 30),
        ]),
    ]);
    expect(lines).toContain(
        "| sample a.cpp | open | 50.0 | 50.0 | 10.0 | 10.0 | 100 | 100 | 30.0 | 30.0 | 2.00× | 3.00× |",
    );
});

test("a failed run's samples stay out of the medians", () => {
    const lines = tables([
        results([
            run("clice", "editLoop", { edit: [100] }),
            run("clice", "editLoop", { edit: [1] }, { error: "timed out after 600000ms" }),
            run("clangd", "editLoop", { edit: [100] }),
        ]),
    ]);
    expect(lines).toContain("| editLoop a.cpp | edit | 100 | 100 | 100 | 100 | 1.00× |");
});

test("every measure a run took is a row, a baseline a ratio of its own", () => {
    const measured = (server: RunResult["server"], scale: number) =>
        run(
            server,
            "index",
            { index: [10 * scale] },
            {
                sizes: { references: 4 },
                values: { "index MB on disk": 2 * scale },
                memory: {
                    peak: { root: 1, child: 2, total: 3 * scale },
                    end: { root: scale, children: scale },
                    roles: {},
                    cpu: 5,
                },
                snapshots: { indexed: { t: 1, total: 7 * scale, roles: {}, cpu: 1 } },
            },
        );
    const lines = tables([
        results([measured("clice", 1), measured("clangd", 2), measured("baseline", 4)]),
    ]);
    const row = (measure: string) =>
        lines.find((line) => line.startsWith(`| index a.cpp | ${measure} |`));
    expect(lines[4]).toBe(
        "| scenario | measure | clice p50 | clice p90 | clangd p50 | clangd p90 | baseline p50 | baseline p90 | clice/clangd | clice/baseline |",
    );
    expect(row("index")).toBe(
        "| index a.cpp | index | 10.0 | 10.0 | 20.0 | 20.0 | 40.0 | 40.0 | 0.50× | 0.25× |",
    );
    expect(row("references size")).toBe(
        "| index a.cpp | references size | 4.0 | 4.0 | 4.0 | 4.0 | 4.0 | 4.0 | 1.00× | 1.00× |",
    );
    expect(row("index MB on disk")?.split(" | ")[2]).toBe("2.0");
    expect(row("peak memory MB")?.split(" | ")[4]).toBe("6.0");
    expect(row("end memory MB")?.split(" | ")[6]).toBe("8.0");
    expect(row("CPU s")?.split(" | ")[2]).toBe("5.0");
    expect(row("memory indexed MB")?.split(" | ")[2]).toBe("7.0");
});

test("another machine's runs get a table of their own", () => {
    const other = results([run("clice", "editLoop", { edit: [5] })]);
    other.env = { ...other.env, cpuModel: "another cpu" };
    const lines = tables([results([run("clice", "editLoop", { edit: [7] })]), other]);
    expect(lines.filter((line) => line === "### p/editing")).toHaveLength(2);
    expect(lines.filter((line) => line.startsWith("| editLoop a.cpp | edit |"))).toEqual([
        "| editLoop a.cpp | edit | 7.0 | 7.0 |",
        "| editLoop a.cpp | edit | 5.0 | 5.0 |",
    ]);
});

test("a perf job's summary: its new failures, the known ones, then the tables", () => {
    const text = perfReport({
        ...results([run("clice", "editLoop", { edit: [7] })]),
        failures: [
            {
                kind: "crash",
                file: "a.cpp",
                request: "editLoop",
                detail: "clice, run 1: clice exited with SIGSEGV",
                repro: "run it",
            },
        ],
        known: [
            {
                kind: "hang",
                file: "a.cpp, 1000 MB",
                request: "limitedIndex",
                detail: "clice, run 1: not idle",
                repro: "r",
                issue: "10-23#25",
            },
        ],
    });
    expect(text).toContain("# perf p/editing on linux: 1 new failures, 1 known");
    expect(text).toContain("## Runs clice crashed in (1)\n\n### a.cpp, editLoop");
    expect(text).toContain("## Known failures (1)\n\n### 10-23#25: a.cpp, 1000 MB, limitedIndex");
    expect(text).toContain("| editLoop a.cpp | edit | 7.0 | 7.0 |");
});

test("a run's errors name its failure: a crash before the hang it caused", () => {
    const options = {
        job: { kind: "perf", name: "p/editing" },
        clice: "/c",
        dir: "/d",
    } as unknown as PerfOptions;
    const kind = (error: string) =>
        failureOf(run("clice", "editLoop", {}, { error }), options).kind;
    expect(kind("timed out after 600000ms: hover; clice exited with SIGSEGV")).toBe("crash");
    expect(kind("clice logged: [error] clice worker crashed in: clice/worker/hover a.cpp")).toBe(
        "crash",
    );
    expect(kind("timed out after 600000ms: hover; clice exited with SIGKILL")).toBe("hang");
    expect(kind("clice: the background index is not idle after 30 minutes")).toBe("hang");
    expect(kind("Request textDocument/hover failed with message: no")).toBe("server");
    expect(failureOf(run("clice", "editLoop", {}, { error: "x" }), options).repro).toBe(
        "CLICE_EXECUTABLE=/c pixi run node tools/ecosystem/run.ts perf p/editing --dir /d --scenario editLoop",
    );
});

test("what clice spends a tenth more of than the baseline, above the noise", () => {
    const index = (server: RunResult["server"], ms: number, hover: number, variant?: string) =>
        run(
            server,
            "index",
            { index: [ms], hover: [hover] },
            {
                sizes: { hover: 900 },
                values: { "indexed files": server === "clice" ? 30 : 20, "index MB on disk": 50 },
                ...(variant === undefined ? {} : { variant }),
            },
        );
    const found = slower(
        results([
            index("clice", 120, 5),
            index("baseline", 100, 4),
            index("clice", 320, 50, "no PCH"),
            index("baseline", 300, 50, "no PCH"),
        ]),
    );
    expect(found).toEqual([
        {
            scenario: "index",
            name: "index a.cpp",
            measure: "index",
            clice: "120",
            baseline: "100",
            clicePerBaseline: 1.2,
        },
    ]);
    const more = slower(
        results([index("clice", 120, 50, "no PCH"), index("baseline", 100, 40, "no PCH")]),
    );
    expect(more.map(({ measure }) => measure)).toEqual(["index (no PCH)", "hover (no PCH)"]);
    expect(slower(results([index("clice", 500, 50), index("clangd", 100, 40)]))).toEqual([]);
});

test("a perf job's summary lists what clice spends more of, with the command per scenario", () => {
    const text = perfReport({
        ...results([
            run("clice", "index", { index: [120] }),
            run("baseline", "index", { index: [100] }),
        ]),
        slower: [
            {
                scenario: "index",
                name: "index a.cpp",
                measure: "index",
                clice: "120",
                baseline: "100",
                clicePerBaseline: 1.2,
                repro: "run index",
            },
        ],
    });
    expect(text).toContain("# perf p/editing on linux: green, 1 slower");
    expect(text).toContain(
        "## Slower or heavier than the baseline (1)\n\n| scenario | measure | clice p50 | baseline p50 | clice/baseline |\n|---|---|---:|---:|---:|\n| index a.cpp | index | 120 | 100 | 1.20× |\n\nReproduce:\n\n- index: `run index`",
    );
});

test("a run against the baseline alone is run again so", () => {
    const options = {
        job: { kind: "perf", name: "p/editing" },
        clice: "/c",
        dir: "/d",
        baseline: "/b/clice",
        clangd: false,
    } as unknown as PerfOptions;
    expect(failureOf(run("clice", "index", {}, { error: "x" }), options).repro).toBe(
        "CLICE_EXECUTABLE=/c pixi run node tools/ecosystem/run.ts perf p/editing --dir /d --scenario index --baseline /b/clice --no-clangd",
    );
});
