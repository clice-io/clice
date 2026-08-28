/// End-to-end check of the anomaly reporting machinery.
///
/// Kills a worker process and verifies the master reports `[anomaly:WorkerCrash]`
/// both via window/logMessage and in its log file — the same channels
/// assertNoAnomaly() watches in every other test's teardown.

import * as fs from "node:fs";
import * as path from "node:path";
import { anomaliesInLogFiles, waitUntil } from "@clice/tools/client";
import { expect, test } from "../fixtures.ts";

export function childPids(parentPid: number): number[] {
    const pids: number[] = [];
    for (const entry of fs.readdirSync("/proc")) {
        if (!/^\d+$/.test(entry)) {
            continue;
        }
        let stat: string;
        try {
            stat = fs.readFileSync(`/proc/${entry}/stat`, "utf8");
        } catch {
            continue;
        }
        // /proc/<pid>/stat: pid (comm) state ppid ...
        const ppid = Number(
            stat
                .slice(stat.lastIndexOf(")") + 1)
                .trim()
                .split(/\s+/)[1],
        );
        if (ppid === parentPid) {
            pids.push(Number(entry));
        }
    }
    return pids;
}

test.skipIf(process.platform !== "linux")("worker crash reported", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write("main.cpp", "int main() { return 0; }\n");
    workspace.writeCDB(["main.cpp"]);

    // Debug builds abort on anomalies by design; disable the trap so this
    // test can observe the report-and-continue (Release) behavior everywhere.
    // The crash is intentional, so the session opts out of the anomaly gate.
    process.env["CLICE_ANOMALY_NO_TRAP"] = "1";
    let client;
    try {
        client = session.spawn(workspace, { allowAnomaly: true });
        await client.initialize(workspace);
    } finally {
        delete process.env["CLICE_ANOMALY_NO_TRAP"];
    }

    try {
        await client.openAndWait("main.cpp");

        const serverPid = client.child.pid!;
        const workers = childPids(serverPid);
        expect(workers.length, "server should have spawned worker processes").toBeGreaterThan(0);

        // The crash handler is installed when the worker opens its log file;
        // killing a worker before that point produces no backtrace. Wait
        // until every spawned worker has a log file — the log names carry
        // worker names, not pids, so per-process association is impossible
        // and readiness of the whole fleet is the usable condition.
        const logsDir = workspace.path(".clice/logs");
        await waitUntil(
            () => {
                const names = fs.existsSync(logsDir)
                    ? fs.readdirSync(logsDir, { recursive: true, encoding: "utf8" })
                    : [];
                return (
                    names.filter(
                        (name) => name.endsWith(".log") && path.basename(name) !== "master.log",
                    ).length >= workers.length
                );
            },
            {
                timeout: 10_000,
                interval: 200,
                description: "every worker to open its log file",
            },
        );
        // SIGABRT: dies via the same signal path as clice's own Debug traps,
        // exercises the crash handler, and is not intercepted by ASan
        // (handle_abort=0), unlike SIGSEGV which ASan turns into its own
        // report and a plain exit(1).
        process.kill(workers[0]!, "SIGABRT");

        await waitUntil(() => client.anomaliesInLogMessages().includes("WorkerCrash"), {
            timeout: 10_000,
            interval: 200,
            description: "the worker crash anomaly message",
        });
        expect(
            client.anomaliesInLogMessages().includes("WorkerCrash"),
            `expected WorkerCrash anomaly, got messages: ${JSON.stringify(
                client.logMessages.map((m) => m.message),
            )}`,
        ).toBe(true);
    } finally {
        await client.shutdown();
    }

    // The same marker must be greppable in the log files (this is what
    // assertNoAnomaly relies on for worker-side anomalies).
    expect(anomaliesInLogFiles(workspace.root).some((entry) => entry.includes("WorkerCrash"))).toBe(
        true,
    );

    // The abort also exercises the crash handler: the worker's backtrace
    // must land in its own log file — and ONLY there, never relayed into
    // the master log.
    const logsDir = workspace.path(".clice/logs");
    const logNames = fs.readdirSync(logsDir, { recursive: true, encoding: "utf8" });
    const workerTexts = logNames
        .filter((name) => name.endsWith(".log") && path.basename(name) !== "master.log")
        .map((name) => fs.readFileSync(path.join(logsDir, name), "utf8"));
    expect(
        workerTexts.some((text) => text.includes("CRASH STACK TRACE")),
        "worker crash backtrace should be written to its log file",
    ).toBe(true);
    expect(
        workerTexts.some((text) => /main executable base: 0x[0-9a-fA-F]+/.test(text)),
        "crash trace should record the executable base for offline symbolization",
    ).toBe(true);
    expect(
        workerTexts.some((text) => /^clice \S+ \S+$/m.test(text)),
        "crash trace should stamp the version/target line",
    ).toBe(true);
    const masterTexts = logNames
        .filter((name) => path.basename(name) === "master.log")
        .map((name) => fs.readFileSync(path.join(logsDir, name), "utf8"));
    expect(
        masterTexts.length > 0 &&
            masterTexts.every(
                (text) => !text.includes("CRASH STACK TRACE") && !text.includes("Stack dump"),
            ),
        "worker backtrace must not leak into the master log",
    ).toBe(true);
});
