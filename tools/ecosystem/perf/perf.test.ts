/// Tests for the perf tables: medians per server, ratios of the medians,
/// a scenario over many files pooled by its files' ratios, and a variant
/// compared with the same variant of the other server.

import { expect, test } from "vitest";
import { tables } from "./compare.ts";
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
