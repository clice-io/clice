import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { expect, test } from "vitest";
import { anomalyGateFailure, processGateFailures } from "../../tools/process_gate.ts";

test("process gate accepts a clean exit with complete stderr", () => {
    expect(
        processGateFailures({
            exitCode: 0,
            signalCode: null,
            stderrText: "",
            stderrComplete: true,
            stderrDrainedFromStart: true,
        }),
    ).toEqual([]);
});

test("process gate rejects every incomplete or unsafe process outcome", () => {
    expect(
        processGateFailures({
            exitCode: null,
            signalCode: "SIGABRT",
            stderrText:
                "[logging] dropped 3 stderr line(s): client not draining\n" +
                "runtime error: invalid value",
            stderrComplete: false,
            stderrFailure: "timed out",
            stderrDrainedFromStart: true,
        }),
    ).toEqual([
        "server exited from signal SIGABRT",
        "stderr pump did not complete: timed out",
        "stderr mirror shed lines despite a draining client",
        "server stderr contains sanitizer/runtime error output",
    ]);
});

test("anomaly gate ignores logs predating the session", () => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "process-gate-"));
    try {
        const stale = path.join(root, ".clice", "logs", "old-session");
        fs.mkdirSync(stale, { recursive: true });
        const staleLog = path.join(stale, "worker.log");
        fs.writeFileSync(staleLog, "[anomaly:WorkerCrash] leftover\n");
        const preexisting = new Set([staleLog]);

        expect(anomalyGateFailure([], root, preexisting)).toBeNull();

        const fresh = path.join(root, ".clice", "logs", "new-session");
        fs.mkdirSync(fresh, { recursive: true });
        fs.writeFileSync(path.join(fresh, "master.log"), "[anomaly:MasterCrash] current\n");

        const failure = anomalyGateFailure([], root, preexisting);
        expect(failure).toContain("MasterCrash");
        expect(failure).not.toContain("WorkerCrash");
    } finally {
        fs.rmSync(root, { recursive: true });
    }
});

test("anomaly gate scans notification IDs and workspace logs", () => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "process-gate-"));
    try {
        const logs = path.join(root, ".clice", "logs", "session");
        fs.mkdirSync(logs, { recursive: true });
        fs.writeFileSync(
            path.join(logs, "worker.log"),
            "[anomaly:WorkerCrash] worker failed\n=== CRASH STACK TRACE ===\nframe 0\n",
        );

        const failure = anomalyGateFailure(["MasterCrash"], root);
        expect(failure).toContain("MasterCrash");
        expect(failure).toContain("WorkerCrash");
        expect(failure).toContain("=== CRASH STACK TRACE ===");
    } finally {
        fs.rmSync(root, { recursive: true });
    }
});
