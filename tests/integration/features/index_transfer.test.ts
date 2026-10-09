/// An index too large for an IPC message reaches the master through a
/// transfer file, and is dropped with a warning when there is no cache store
/// to hold one. CLICE_TEST_MAX_INDEX_BYTES lowers the inline limit so every
/// index here takes that route.

import { runProcess } from "@clice/tools/client";
import { cliceExecutable, expect, test } from "../fixtures.ts";

const HOOK = { CLICE_TEST_MAX_INDEX_BYTES: "1024" };
const FUNCTIONS = Array.from({ length: 200 }, (_, i) => `int function_${i}() { return ${i}; }`);
const BIG = `${FUNCTIONS.join("\n")}\nint use() { return function_0(); }\n`;

test("batch index via file", async ({ session }) => {
    const exe = cliceExecutable();
    const ws = session.tmpdir();
    ws.write("a.cpp", `int alpha() { return 1; }\n${BIG}`);
    ws.write("b.cpp", "int alpha();\nint beta() { return alpha(); }\n");
    ws.writeCDB(["a.cpp", "b.cpp"]);

    const env = { ...process.env, ...HOOK };
    const run = await runProcess(exe, ["index", "--workspace", ws.root], {
        env,
        timeout: 120_000,
    });
    expect(run.status, run.stderr).toBe(0);
    expect(run.stdout).toContain("Indexed 2 translation units");

    const shown = await runProcess(
        exe,
        ["index", "--workspace", ws.root, "--show-symbol", "alpha"],
        {
            env,
            timeout: 60_000,
        },
    );
    expect(shown.status, shown.stderr).toBe(0);
    expect(shown.stdout).toContain("reference files=2");
});
