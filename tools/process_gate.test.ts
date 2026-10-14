import { expect, test } from "vitest";
import { processGateFailures, serverStderrExcerpt } from "./process_gate.ts";

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

test("stderr excerpt keeps the runtime's abort messages", () => {
    const aborts = [
        "Fatal glibc error: malloc.c:2601 (sysmalloc): assertion failed: (old_top == initial_top (av) && old_size == 0)",
        "*** stack smashing detected ***: terminated",
        "[stateless-0] free(): invalid pointer",
        "double free or corruption (out)",
        "clice: llvm/lib/Support/APInt.cpp:281: void llvm::APInt::foo(): Assertion `BitWidth == RHS.BitWidth' failed.",
        "Assertion failed: (index < size), function at, file vector, line 1426.",
        "libc++abi: terminating due to uncaught exception of type std::bad_alloc",
        "terminate called after throwing an instance of 'std::length_error'",
        "LLVM ERROR: out of memory",
    ];
    const stderr = ["[info] Server ready", ...aborts, "[debug] tick", "PLEASE submit a bug report"];
    expect(serverStderrExcerpt(stderr.join("\n")).split("\n")).toEqual(aborts);
});
