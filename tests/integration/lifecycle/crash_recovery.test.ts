/// Crash-recovery of background indexing.
///
/// Kills stateless workers while an indexing round is in flight and verifies
/// the round still converges: an outside kill names no request, so the
/// in-flight files are lost rather than blamed, the indexer requeues them,
/// and a follow-up round indexes every file. The second test darkens the
/// whole pool (crash budget exhausted) and verifies the round parks until
/// revival instead of spinning requeues (#611). The third pins that a file
/// whose indexing crashed a worker waits for a change instead of being
/// retried.

import { MTIME_GRANULARITY, sleep, waitUntil, type CliceClient } from "@clice/tools/client";
import { expect, test } from "../fixtures.ts";

const FILE_COUNT = 12;
const KILL_FILE_COUNT = 3;
const OUTAGE_RESPONSE_TIMEOUT = 15_000;

async function indexedFunctions(client: CliceClient): Promise<Set<string>> {
    const result = await client.workspaceSymbols("func_");
    return new Set((result ?? []).map((s) => s.name));
}

test.skipIf(process.platform !== "linux")(
    "crash during indexing",
    { timeout: 360_000 },
    async ({ session }) => {
        const workspace = session.tmpdir();
        const files: string[] = [];
        for (let i = 0; i < KILL_FILE_COUNT; i++) {
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

        // Kill the worker an index run is in flight on, as soon as one
        // takes it: a background compile runs niced, so waiting for its
        // result instead would let a loaded machine starve the wait.
        const killed = await waitUntil(
            () => {
                for (let i = 0; i < 16; i++) {
                    const name = `SL-${i}`;
                    if (workspace.log(`${name}.log`).includes("TURun request")) {
                        for (const pid of client.workerPids(`${name}\0`)) {
                            process.kill(pid, "SIGKILL");
                        }
                        return true;
                    }
                }
                return false;
            },
            {
                timeout: 30_000,
                interval: 50,
                description: "an index run to reach a stateless worker",
            },
        );
        expect(killed, "indexing never started or no stateless worker found").toBe(true);

        // The files that were in flight on the killed worker must be
        // requeued and indexed by a follow-up round: every function
        // eventually appears in the project index.
        const expected = new Set(Array.from({ length: KILL_FILE_COUNT }, (_, i) => `func_${i}`));
        let found = new Set<string>();
        await waitUntil(
            async () => {
                found = await indexedFunctions(client);
                return [...expected].every((name) => found.has(name));
            },
            {
                timeout: 240_000,
                interval: 1_000,
                description: "every translation unit to be reindexed after a worker crash",
            },
        );
        const missing = [...expected].filter((f) => !found.has(f)).sort();
        expect(
            [...expected].every((f) => found.has(f)),
            `missing after crash: ${JSON.stringify(missing)}`,
        ).toBe(true);

        // The kill landed on an index run in flight: the premise under test.
        expect(workspace.log("master.log")).toContain("Worker died while indexing");
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

        // Kill the slot on sight until the pool reports the budget as spent
        // (a fast-crash streak past max_crash_streak); respawn backoff caps
        // at ~1s, so a few seconds of killing cover every respawn.
        let kills = 0;
        await waitUntil(
            () => {
                for (const pid of client.workerPids("SL-")) {
                    try {
                        process.kill(pid, "SIGKILL");
                        kills += 1;
                    } catch {
                        // Already reaped.
                    }
                }
                return workspace.log("master.log").includes("exceeded crash budget");
            },
            {
                timeout: 30_000,
                interval: 200,
                description: "the stateless worker pool to exhaust its crash budget",
            },
        );
        expect(kills, "no stateless worker was ever seen").toBeGreaterThanOrEqual(3);
        expect(
            workspace.log("master.log").includes("exceeded crash budget"),
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
        // must resume and finish every file: lost runs requeue past the
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
        const requeues = workspace.log("master.log").match(/No stateless workers available/g);
        expect((requeues ?? []).length).toBeLessThanOrEqual(FILE_COUNT);
    },
);

// A file whose own index run crashes its worker is not requeued: the same
// bytes would crash the next run too. It is retried once it changes.
test.skipIf(process.platform !== "linux")(
    "index crash waits for change",
    { timeout: 240_000 },
    async ({ session }) => {
        const workspace = session.tmpdir();
        workspace.write("poison.cpp", "int poison_fn() { return 1; }\n");
        workspace.write("healthy.cpp", "int healthy_fn() { return 2; }\n");
        workspace.write("main.cpp", "int main() { return 0; }\n");
        workspace.writeCDB(["poison.cpp", "healthy.cpp", "main.cpp"]);
        const run = `tuRun ${workspace.displayPath("poison.cpp")}`;
        const client = session.spawn(workspace, {
            allowAnomaly: true,
            env: { CLICE_ANOMALY_NO_TRAP: "1", CLICE_TEST_CRASH_REQUEST: run },
        });
        await client.initialize(workspace);
        await client.openAndWait("main.cpp");

        const crashes = () => workspace.workerCrashes(run);
        const symbols = async () =>
            new Set(((await client.workspaceSymbols("_fn")) ?? []).map((s) => s.name));
        await waitUntil(async () => (await symbols()).has("healthy_fn") && crashes() === 1, {
            timeout: 120_000,
            interval: 500,
            description: "the round to index healthy.cpp and crash on poison.cpp",
        });
        await waitUntil(() => workspace.log("master.log").includes("Index giving up on"), {
            timeout: 30_000,
            interval: 200,
            description: "the indexer to give up on poison.cpp",
        });
        expect(workspace.log("master.log")).toContain("[anomaly:WorkerCrash]");

        // A change is the retry.
        await sleep(MTIME_GRANULARITY);
        workspace.write("poison.cpp", "int poison_fn() { return 300; }\n");
        await client.poll("workspace");
        await waitUntil(() => crashes() === 2, {
            timeout: 120_000,
            interval: 500,
            description: "the changed file to be indexed again",
        });
        expect((await symbols()).has("poison_fn")).toBe(false);
    },
);
