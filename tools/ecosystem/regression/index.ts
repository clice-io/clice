/// `clice index` over a prepared project, from a cold cache: its JSON
/// report, its time and peak memory, the size of the index it wrote, and
/// the units in flight when it stopped making progress.

import { spawn } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { cliceExecutable } from "../../client/session.ts";
import { watchTree, type PeakMemory } from "../memory.ts";
import { cacheBytes } from "../perf/servers.ts";

/// `clice index --json`'s report, as far as the run reads it. A run handed
/// to a running server reports only `failed`.
export interface IndexReport {
    completed?: boolean;
    unsaved: boolean;
    damaged?: boolean;
    indexedTus: number;
    shardCount: number;
    shardBytes: number;
    symbolCount: number;
    failed: string[];
    /// The units whose worker ran out of memory alone, at `memory` bytes.
    tooBig?: { unit: string; memory: number }[];
    compiledWithErrors: {
        unit: string;
        errors: { file: string; line: number; column: number; message: string }[];
    }[];
}

export interface IndexRun {
    seconds: number;
    status: number | null;
    signal: NodeJS.Signals | null;
    /// Absent when the command printed no report: it died, or never got to
    /// the end of a run.
    report?: IndexReport;
    memory?: PeakMemory;
    /// Bytes of the cache directory the run left.
    cacheBytes: number;
    /// The units a run that stopped making progress was working on.
    hung?: string[];
    /// The progress a run stopped at its time bound had made.
    overTime?: string;
    /// The last lines the command wrote to stderr.
    stderr: string;
}

/// How long a run may go without finishing a unit before it counts as
/// hung: the slowest unit of the projects takes about two minutes.
const HANG_MS = 30 * 60_000;

/// How long a run may take at all, so that the job ends within the runner's
/// six hours: the slowest part takes under two.
const INDEX_MS = 3 * 60 * 60_000;

/// How the session log ends a unit's attempt (src/sched/index/pump.cpp).
const ENDED =
    /\] (?:Index failed for|Index dropped for removed file|Index failure for superseded content of|Index giving up on|Worker died while indexing|Index requeued for) (\S+?)[:;]?(?:\s|$)/;

/// The units the session log shows begun and whose attempt has not ended.
export function unitsInFlight(log: string): string[] {
    const started = new Set<string>();
    for (const line of log.split(/\r?\n/)) {
        const begun = /\] Indexing (\S+)$/.exec(line);
        const done = /progress=\d+\/\d+ file=(\S+)/.exec(line) ?? ENDED.exec(line);
        if (begun?.[1] !== undefined) {
            started.add(begun[1]);
        } else if (done?.[1] !== undefined) {
            started.delete(done[1]);
        }
    }
    return [...started].sort();
}

function sessionLog(checkout: string): string {
    const logs = path.join(checkout, ".clice", "logs");
    const newest = fs.existsSync(logs) ? fs.readdirSync(logs).sort().at(-1) : undefined;
    const log = newest === undefined ? "" : path.join(logs, newest, "index.log");
    return log !== "" && fs.existsSync(log) ? fs.readFileSync(log, "utf8") : "";
}

export function runIndex(checkout: string): Promise<IndexRun> {
    const started = Date.now();
    const child = spawn(
        cliceExecutable(),
        ["index", "--json", "--workspace", checkout, "--jobs", String(os.availableParallelism())],
        { stdio: ["ignore", "pipe", "pipe"] },
    );
    const memory = child.pid === undefined ? undefined : watchTree(child.pid);
    let stdout = "";
    let stderr = "";
    let progress = "";
    let hung: string[] | undefined;
    let overTime: string | undefined;
    let stall: NodeJS.Timeout | undefined;
    // Time is the feature: the run's bound, past which it is stopped as
    // the hang detection below stops it.
    const bound = setTimeout(() => {
        overTime = progress;
        clearTimeout(stall);
        child.kill("SIGINT");
        // Hang detection: an interrupted run that does not wind down.
        stall = setTimeout(() => child.kill("SIGKILL"), 5 * 60_000);
    }, INDEX_MS);
    const arm = () => {
        clearTimeout(stall);
        // Hang detection: no unit finished for half an hour.
        stall = setTimeout(() => {
            hung = unitsInFlight(sessionLog(checkout));
            child.kill("SIGINT");
            // Hang detection: an interrupted run that does not wind down.
            stall = setTimeout(() => child.kill("SIGKILL"), 5 * 60_000);
        }, HANG_MS);
    };
    arm();
    child.stdout.setEncoding("utf8").on("data", (chunk: string) => {
        stdout += chunk;
    });
    child.stderr.setEncoding("utf8").on("data", (chunk: string) => {
        stderr = (stderr + chunk).slice(-20_000);
        for (const line of chunk.split("\n")) {
            const counts = /^progress (\d+\/\d+) units/.exec(line)?.[1];
            if (
                counts !== undefined &&
                counts !== progress &&
                hung === undefined &&
                overTime === undefined
            ) {
                progress = counts;
                process.stderr.write(`${line}\n`);
                arm();
            }
        }
    });
    return new Promise((resolve, reject) => {
        child.on("error", reject);
        child.on("close", (status, signal) => {
            clearTimeout(stall);
            clearTimeout(bound);
            const peak = memory?.stop().peak;
            let report: IndexReport | undefined;
            try {
                report = JSON.parse(stdout) as IndexReport;
            } catch {
                report = undefined;
            }
            resolve({
                seconds: (Date.now() - started) / 1000,
                status,
                signal,
                ...(report === undefined ? {} : { report }),
                ...(peak === undefined ? {} : { memory: peak }),
                cacheBytes: cacheBytes(checkout, checkout, "clice").disk,
                ...(hung === undefined ? {} : { hung }),
                ...(overTime === undefined ? {} : { overTime }),
                stderr: stderr.split("\n").slice(-40).join("\n"),
            });
        });
    });
}
