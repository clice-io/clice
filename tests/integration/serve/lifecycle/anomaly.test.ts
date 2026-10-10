/// End-to-end check of the anomaly reporting machinery.
///
/// Kills a worker process and verifies the master reports `[anomaly:WorkerCrash]`
/// both via window/logMessage and in its log file — the same channels
/// assertNoAnomaly() watches in every other test's teardown.

import * as fs from "node:fs";
import * as path from "node:path";
import { anomaliesInLogFiles } from "@clice/tools/client";
import { at, expect, serve } from "../../fixtures.ts";

// Without an index, the hover below waits for the respawned worker:
// rows of the file would answer it at once.
serve
    .files(
        { "main.cpp": "int main() { return 0; }\n" },
        { config: { project: { enable_indexing: false } }, anomalies: true },
    )
    .skipIf(process.platform !== "linux")("worker crash reported", async ({ s }) => {
    // The crash handler is installed when the worker opens its log
    // file, which a worker that compiled has done.
    await s.compiled("main.cpp");
    expect(
        s.client.workerPids().length,
        "server should have spawned worker processes",
    ).toBeGreaterThan(0);
    const [worker] = s.client.workerPids("SF-");
    expect(worker, "the stateful worker").toBeDefined();

    // SIGABRT: dies via the same signal path as clice's own Debug
    // traps, exercises the crash handler, and is not intercepted by
    // ASan (handle_abort=0), unlike SIGSEGV which ASan turns into
    // its own report and a plain exit(1).
    const died = s.client.nextLogMessage((message) => message.includes("[anomaly:WorkerCrash]"));
    process.kill(worker ?? 0, "SIGABRT");
    await died;

    // The document comes back on a respawned worker, which the
    // master starts once it handled the death.
    expect(await s.hover(at("main.cpp", "int m|ain"))).not.toBeNull();
    const client = s.client;
    await s.stop();
    expect(
        client.anomaliesInLogMessages().includes("WorkerCrash"),
        `expected WorkerCrash anomaly, got messages: ${JSON.stringify(
            client.logMessages.map((m) => m.message),
        )}`,
    ).toBe(true);

    // The same marker must be greppable in the log files (this is
    // what assertNoAnomaly relies on for worker-side anomalies).
    expect(
        anomaliesInLogFiles(s.workspace.root).some((entry) => entry.includes("WorkerCrash")),
    ).toBe(true);

    // The abort also exercises the crash handler: the worker's
    // backtrace must land in its own log file — and ONLY there,
    // never relayed into the master log.
    const logsDir = s.workspace.path(".clice/logs");
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
