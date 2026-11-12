/// The results of a job with nothing in them, for the ecosystem's own
/// tests to fill in what each one is about.

import type { PerfResults } from "./perf/results.ts";
import type { Platform } from "./project.ts";
import type { Results } from "./regression/regression.ts";

export function regressionResults(fields: Partial<Results> = {}): Results {
    return {
        job: "p",
        platform: "linux",
        about: "v1",
        commit: "c",
        clice: "k",
        seed: "s",
        compilerRejects: [],
        logErrors: [],
        failures: [],
        known: [],
        unseen: [],
        accepted: [],
        ...fields,
    };
}

export function perfResults(
    fields: Partial<PerfResults> & { platform?: Platform } = {},
): PerfResults {
    const platform = fields.platform ?? "linux";
    return {
        job: "p/editing",
        platform,
        about: "",
        commit: "c",
        clice: "k",
        env: {
            platform,
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
        runs: [],
        failures: [],
        known: [],
        unseen: [],
        slower: [],
        ...fields,
    };
}
