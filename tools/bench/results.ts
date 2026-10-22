/// The results bench.ts writes and compare.ts reads.

import type { Position } from "vscode-languageserver-protocol";
import type { WatchedMemory } from "./memory.ts";
import type { Stats } from "./perf.ts";

export const SCENARIOS = [
    "cold_start",
    "index",
    "warm_start",
    "edit_loop",
    "warm_requests",
] as const;
export type ScenarioName = (typeof SCENARIOS)[number];
export const SERVERS = ["clice", "clangd"] as const;
export type ServerName = (typeof SERVERS)[number];
export const PROBES = ["symbol", "member", "unqualified", "template", "call"] as const;
export type ProbeKind = (typeof PROBES)[number];

/// One run of one scenario by one server.
export interface RunResult {
    server: ServerName;
    scenario: ScenarioName;
    run: number;
    /// Client-observed latencies, milliseconds.
    series: Record<string, number[]>;
    /// What the last answer of each request held (items, locations,
    /// tokens): the times compare only where both servers did the work.
    sizes: Record<string, number>;
    /// Megabytes the server's processes held: the most at once over the
    /// run, and at its end.
    memory?: WatchedMemory;
    /// The index scenario: the most files a round counted, and the
    /// megabytes on disk of the server's index and of its whole cache.
    index?: { files: number; indexMB: number; diskMB: number };
    /// Why the run failed; its series hold what it measured before.
    error?: string;
    /// Whether the OS file cache was dropped before the run.
    dropped?: boolean;
    /// Server-side breakdown from the session's `[perf:*]` log lines
    /// (clice only).
    perf?: Record<string, Stats>;
}

export interface BenchResult {
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
    workspace: string;
    file: string;
    probes: Partial<Record<ProbeKind, Position>>;
    query: string;
    runs: RunResult[];
}
