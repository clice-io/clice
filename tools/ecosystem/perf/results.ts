/// What a perf job records, and what its summary and compare.ts read.

import type { Known, Failure } from "../script.ts";
import type { Platform } from "../project.ts";
import type { PeakMemory, Resident, Sample } from "../memory.ts";
import type { Stats } from "./logs.ts";

/// The servers a job drives: clice under test, clangd, and on request a
/// second clice, the baseline of an A/B of two commits.
export const SERVERS = ["clice", "clangd", "baseline"] as const;
export type ServerName = (typeof SERVERS)[number];

/// The tree's memory at a named moment of a run.
export interface Snapshot {
    /// Seconds since the server started.
    t: number;
    total: number;
    roles: Record<string, number>;
    cpu: number;
}

/// One run of one scenario by one server.
export interface RunResult {
    server: ServerName;
    /// What sets the run apart from the server's usual one: clice without
    /// its PCHs.
    variant?: string;
    scenario: string;
    /// The file the scenario opened, or how many it went over.
    label: string;
    /// The file of this run, in a scenario over many each in a run of its
    /// own.
    file?: string;
    run: number;
    /// Client-observed latencies, milliseconds.
    series: Record<string, number[]>;
    /// What the last answer of each request held (items, locations,
    /// tokens): the times compare only where both servers did the work.
    sizes: Record<string, number>;
    /// Single measures: seconds a churn took to settle, its CPU seconds,
    /// the files an index round counted, megabytes on disk.
    values: Record<string, number>;
    /// The server's process tree: its peaks, at the end, each role's peak,
    /// its CPU seconds.
    memory?: {
        peak: PeakMemory;
        end: Resident;
        roles: Record<string, number>;
        cpu: number;
    };
    snapshots: Record<string, Snapshot>;
    /// Every sample of the tree.
    timeline?: Sample[];
    /// Why the run failed; its series hold what it measured before.
    error?: string;
    /// Whether the OS file cache was dropped before the run.
    dropped?: boolean;
    /// Server-side breakdown from the session's `[perf:*]` log lines
    /// (clice only).
    perf?: Record<string, Stats>;
}

export interface PerfResults {
    job: string;
    platform: Platform;
    about: string;
    /// The project's pinned commit, and clice's.
    commit: string;
    clice: string;
    env: {
        platform: string;
        release: string;
        arch: string;
        cpus: number;
        cpuModel: string;
        memoryMB: number;
        node: string;
        date: string;
    };
    binaries: Partial<Record<ServerName, string>>;
    clangdArgs: string[];
    runs: RunResult[];
    /// The failures of clice's runs the project does not know, those it
    /// knows, and the known issues the job did not see.
    failures: Failure[];
    known: (Failure & { issue: string })[];
    unseen: Known[];
    /// The measures clice spends a tenth more of than the baseline.
    slower: Slower[];
}

/// A measure clice spends more of than the baseline: time, memory, disk or
/// CPU.
export interface Slower {
    scenario: string;
    /// The scenario and its label, as the tables name it.
    name: string;
    measure: string;
    clice: string;
    baseline: string;
    clicePerBaseline: number;
    repro: string;
}
