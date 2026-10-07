/// Integration tests for assembler-with-cpp sources (`.S`): the preprocessor
/// passes a `#` comment line through to the assembler, and both the editor
/// and the batch index must get past it (#791).

import { runProcess } from "@clice/tools/client";
import type { Workspace } from "@clice/tools/workspace";
import { cliceExecutable, expect, test } from "../fixtures.ts";

function writeAssembler(workspace: Workspace): void {
    workspace.write(
        "entry.S",
        "#define SIZE 8\n" +
            "# Copyright 2003 Pavel Machek <pavel@suse.cz\n" +
            ".text\n" +
            "entry:\n" +
            "    .quad SIZE\n",
    );
    workspace.write(
        "compile_commands.json",
        JSON.stringify([
            {
                directory: workspace.root,
                file: workspace.path("entry.S"),
                arguments: ["clang", "-c", workspace.path("entry.S"), "-o", "entry.o"],
            },
        ]),
    );
}

test("open assembler with comments", async ({ session }) => {
    const { client, workspace } = session.tmp();
    writeAssembler(workspace);
    await client.initialize(workspace);
    const [uri] = await client.openAndWait("entry.S");

    const tokens = await client.semanticTokensFull(uri);
    expect(tokens?.data.length).toBeGreaterThan(0);
});

test("index assembler with comments", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.pinCacheDir();
    writeAssembler(workspace);

    const run = await runProcess(
        cliceExecutable(),
        ["index", "--workspace", workspace.root, "--workers", "1"],
        { timeout: 30_000 },
    );
    expect(run.status, `stderr: ${run.stderr}`).toBe(0);
    expect(run.stdout).toContain("Indexed 1 translation unit in");
});
