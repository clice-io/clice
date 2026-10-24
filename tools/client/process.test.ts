import { describe, expect, test } from "vitest";
import { ending } from "./process.ts";

describe("ending", () => {
    const result = { stdout: "", stderr: "" };

    test("a signal is a crash, an exit status is not", () => {
        expect(ending({ ...result, status: null, signal: "SIGSEGV" })).toBe("crashed (SIGSEGV)");
        expect(ending({ ...result, status: 1, signal: null })).toBe("exited 1");
    });

    test("a run stopped before its exit says why", () => {
        const error = new Error("clang timed out after 60000ms");
        expect(ending({ ...result, status: null, signal: "SIGTERM", error })).toBe(error.message);
    });
});
