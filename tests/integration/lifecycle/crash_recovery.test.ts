/// Crash-recovery of background indexing.
///
/// Kills stateless workers while an indexing round is in flight and verifies the
/// round still converges: in-flight files fail with worker_crashed, the indexer
/// requeues them, and a follow-up round indexes every file. The second test
/// darkens the whole pool (crash budget exhausted) and verifies the round parks
/// until revival instead of spinning requeues (#611).

import * as fs from "node:fs";
import * as path from "node:path";
import { sleep, waitUntil, type CliceClient } from "@clice/tools/client";
import { expect, test } from "../fixtures.ts";

const FILE_COUNT = 20;
const OUTAGE_RESPONSE_TIMEOUT = 15_000;

function statelessWorkerPids(serverPid: number): number[] {
    const pids: number[] = [];
    for (const entry of fs.readdirSync("/proc")) {
        if (!/^\d+$/.test(entry)) {
            continue;
        }
        let stat: string;
        let cmdline: Buffer;
        try {
            stat = fs.readFileSync(`/proc/${entry}/stat`, "utf8");
            cmdline = fs.readFileSync(`/proc/${entry}/cmdline`);
        } catch {
            continue;
        }
        const ppid = Number(
            stat
                .slice(stat.lastIndexOf(")") + 1)
                .trim()
                .split(/\s+/)[1],
        );
        if (ppid === serverPid && cmdline.includes("SL-")) {
            pids.push(Number(entry));
        }
    }
    return pids;
}

async function indexedFunctions(client: CliceClient): Promise<Set<string>> {
    const result = await client.workspaceSymbols("func_");
    return new Set((result ?? []).map((s) => s.name));
}

// Recovery after a mid-round worker kill polls up to ~150s under ASan.
test.skipIf(process.platform !== "linux")(
    "crash during indexing",
    { timeout: 300_000 },
    async ({ session }) => {
        const workspace = session.tmpdir();
        // Enough moderately heavy TUs that the indexing round is still in flight
        // when the workers get killed.
        const files: string[] = [];
        for (let i = 0; i < FILE_COUNT; i++) {
            const name = `file_${i}.cpp`;
            workspace.write(
                name,
                `#include <vector>\n#include <string>\n` +
                    `int func_${i}() { return (int)std::string("${i}").size(); }\n`,
            );
            files.push(name);
        }
        workspace.write("main.cpp", "int main() { return 0; }\n");
        workspace.writeCDB([...files, "main.cpp"]);

        // The kills below surface as WorkerCrash anomalies; Debug builds abort on
        // anomalies by design, so disable the trap like anomaly.test does. The
        // crashes are intentional, so the session opts out of the anomaly gate.
        process.env["CLICE_ANOMALY_NO_TRAP"] = "1";
        let client;
        try {
            client = session.spawn(workspace, { allowAnomaly: true });
            await client.initialize(workspace);
        } finally {
            delete process.env["CLICE_ANOMALY_NO_TRAP"];
        }

        await client.openAndWait("main.cpp");

        // Wait until the round demonstrably started (first symbols merged),
        // then kill a stateless worker mid-round.
        const killed = await waitUntil(
            async () => {
                if ((await indexedFunctions(client)).size > 0) {
                    const workers = statelessWorkerPids(client.child.pid!);
                    if (workers.length > 0) {
                        process.kill(workers[0]!, "SIGKILL");
                        return true;
                    }
                }
                return false;
            },
            {
                timeout: 30_000,
                interval: 100,
                description: "indexing to start with a stateless worker available",
            },
        );
        expect(killed, "indexing never started or no stateless worker found").toBe(true);

        // The files that were in flight on the killed worker must be
        // requeued and indexed by a follow-up round: every function
        // eventually appears in the project index.
        const expected = new Set(Array.from({ length: FILE_COUNT }, (_, i) => `func_${i}`));
        // Budgeted for the Debug/ASan CI runners: 20 single-worker ASan
        // compiles plus a crash respawn and one round boundary for the
        // requeued file measure ~130s there (~60s locally).
        let found = new Set<string>();
        await waitUntil(
            async () => {
                found = await indexedFunctions(client);
                return [...expected].every((name) => found.has(name));
            },
            {
                timeout: 180_000,
                interval: 1_000,
                description: "every translation unit to be reindexed after a worker crash",
            },
        );
        const missing = [...expected].filter((f) => !found.has(f)).sort();
        expect(
            [...expected].every((f) => found.has(f)),
            `missing after crash: ${JSON.stringify(missing)}`,
        ).toBe(true);
    },
);

// Regression for #611: with every stateless slot's crash budget burnt, the
// dispatch loop must park until the pool revives a slot — not spin the same
// requeued files through instant worker-unavailable failures (the incident
// produced a ~986 GiB master.log with a [51294/1] progress numerator).
test.skipIf(process.platform !== "linux")(
    "pool outage parks indexing",
    { timeout: 300_000 },
    async ({ session }) => {
        const workspace = session.tmpdir();
        const files: string[] = [];
        for (let i = 0; i < FILE_COUNT; i++) {
            const name = `file_${i}.cpp`;
            workspace.write(
                name,
                `#include <string>\n` +
                    `int func_${i}() { return (int)std::string("${i}").size(); }\n`,
            );
            files.push(name);
        }
        workspace.write("main.cpp", "int main() { return 0; }\n");
        workspace.writeCDB([...files, "main.cpp"]);

        // One stateless slot only, so exhausting its crash budget darkens
        // the whole pool. The kills surface as WorkerCrash anomalies.
        process.env["CLICE_ANOMALY_NO_TRAP"] = "1";
        let client;
        try {
            client = session.spawn(workspace, { allowAnomaly: true });
            await client.initialize(workspace, {
                initializationOptions: {
                    project: {
                        stateless_worker_count: 1,
                        min_stateless_worker_count: 1,
                        max_stateless_worker_count: 1,
                        idle_timeout_ms: 10,
                    },
                },
            });
        } finally {
            delete process.env["CLICE_ANOMALY_NO_TRAP"];
        }

        await client.openAndWait("main.cpp");

        const logsDir = workspace.path(".clice/logs");
        const masterLog = () =>
            fs
                .readdirSync(logsDir, { recursive: true, encoding: "utf8" })
                .filter((name) => path.basename(name) === "master.log")
                .map((name) => fs.readFileSync(path.join(logsDir, name), "utf8"))
                .join("");

        // Kill the slot on sight until the pool reports the budget as spent
        // (a fast-crash streak past max_crash_streak); respawn backoff caps
        // at ~1s, so a few seconds of killing cover every respawn.
        let kills = 0;
        await waitUntil(
            () => {
                for (const pid of statelessWorkerPids(client.child.pid!)) {
                    try {
                        process.kill(pid, "SIGKILL");
                        kills += 1;
                    } catch {
                        // Already reaped.
                    }
                }
                return masterLog().includes("exceeded crash budget");
            },
            {
                timeout: 30_000,
                interval: 200,
                description: "the stateless worker pool to exhaust its crash budget",
            },
        );
        expect(kills, "no stateless worker was ever seen").toBeGreaterThanOrEqual(3);
        expect(
            masterLog().includes("exceeded crash budget"),
            "the pool never went dark — the outage under test did not happen",
        ).toBe(true);

        // Dark window: the master must stay responsive while the round is
        // parked on the capacity signal — fail loudly here instead of via
        // the test timeout if it wedged.
        const during = await Promise.race([
            indexedFunctions(client),
            sleep(OUTAGE_RESPONSE_TIMEOUT).then(() => null),
        ]);
        expect(during, "master unresponsive during the outage").not.toBeNull();

        // The revival cooldown (30s) re-arms the slot and the parked round
        // must resume and finish every file: crash requeues land past the
        // round snapshot, so no single file burns its budget.
        const expected = new Set(Array.from({ length: FILE_COUNT }, (_, i) => `func_${i}`));
        let found = new Set<string>();
        await waitUntil(
            async () => {
                found = await indexedFunctions(client);
                return [...expected].every((name) => found.has(name));
            },
            {
                timeout: 150_000,
                interval: 1_000,
                description: "every translation unit to be indexed after pool revival",
            },
        );
        const missing = [...expected].filter((f) => !found.has(f)).sort();
        expect(
            [...expected].every((f) => found.has(f)),
            `missing after outage (had ${during?.size} during): ${JSON.stringify(missing)}`,
        ).toBe(true);

        // The spin itself: parked dispatch sends nothing, so the outage may
        // produce at most a handful of worker-unavailable requeues — the
        // incident produced them at an unbounded rate.
        const requeues = masterLog().match(/No stateless workers available/g);
        expect((requeues ?? []).length).toBeLessThanOrEqual(FILE_COUNT);
    },
);
