import { spawn } from "node:child_process";
import { once } from "node:events";
import { expect } from "vitest";
import { test } from "@clice/tools/testing";
import { cliceExecutable } from "@clice/tools/session";

// A command that crashes, as clang compiling in its own process can, prints
// the crash's trace to stderr, from the frame the crash interrupted, and
// ends as the crash ends it. Its transcript would hold the trace's addresses,
// so it runs outside `t.cli`.

test("crash in a command", async (t) => {
    const command = spawn(cliceExecutable(), ["inspect", "document-symbols", "main.cpp"], {
        cwd: t.root,
        env: { ...process.env, CLICE_TEST_PRAGMA_CRASH: "1" },
        stdio: ["ignore", "ignore", "pipe"],
    });
    let stderr = "";
    command.stderr.on("data", (chunk: Buffer) => {
        stderr += chunk.toString();
    });
    const [code, signal] = (await once(command, "close")) as [number | null, NodeJS.Signals | null];
    if (process.platform === "win32") {
        // The NTSTATUS of the exception the trap raised.
        expect(code !== null && code >= 0x8000_0000).toBe(true);
    } else {
        expect(["SIGILL", "SIGTRAP"]).toContain(signal);
    }
    expect(stderr).toMatch(
        /=== CRASH STACK TRACE ===\r?\nclice \S+ \S+( https:\/\/github\.com\/clice-io\/clice\/tree\/[0-9a-f]{40})?\r?\n/,
    );
    expect(stderr).toMatch(
        /^0x[0-9a-fA-F]+, .+\(0x[0-9a-fA-F]+\) \+ 0x[0-9a-fA-F]+ byte\(s\) \(interrupted\)\r?$/m,
    );
});
