import { expect, test } from "vitest";
import { processGateFailures } from "./process_gate.ts";

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
