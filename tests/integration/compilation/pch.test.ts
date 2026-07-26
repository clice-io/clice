/// Integration tests for PCH (precompiled header) functionality in MasterServer.

import * as path from "node:path";
import { assertCleanCompile, assertNoErrors, waitForRecompile } from "../../tools/checks.ts";
import { cliceTest, expect } from "../../tools/fixtures.ts";

const test = cliceTest("pch_test");

test("pch diagnostics on open", async ({ client, workspace }) => {
    /// Opening a file with #include should trigger PCH build and return clean diagnostics.
    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    expect(client.diagnostics.has(uri)).toBe(true);
    // main.cpp is well-formed, so diagnostics list should be empty (no errors).
    assertCleanCompile(client, uri);
    client.close(uri);
});

test("pch body edit triggers recompile", async ({ client, workspace }) => {
    /// Editing only the body (not the preamble) should trigger recompilation.
    const [uri, content] = await client.openAndWait(path.join(workspace, "main.cpp"));

    // Edit only the function body — preamble (#include "common.h") unchanged.
    const newContent = content.replace("return result;", "return result + 1;");
    client.change(uri, 1, newContent);
    // Send hover to trigger recompilation via pull-based model.
    await waitForRecompile(client, uri, 30_000);
    expect(client.diagnostics.has(uri)).toBe(true);
    client.close(uri);
});

test("no pch for no includes", async ({ client, workspace }) => {
    /// A file with no #include directives should compile without PCH.
    const [uri] = await client.openAndWait(path.join(workspace, "no_includes.cpp"));
    expect(client.diagnostics.has(uri)).toBe(true);
    assertCleanCompile(client, uri);
    client.close(uri);
});

test("hover on local symbol", async ({ client, workspace }) => {
    /// Hover on a locally defined symbol should work when PCH is active.
    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));

    // Hover over "add" on line 2 (0-indexed): "int add(int a, int b) {"
    const result = await client.hoverAt(uri, 2, 4);
    expect(result).not.toBeNull();
    client.close(uri);
});

test("completion with pch", async ({ client, workspace }) => {
    /// Completion should see symbols from PCH headers.
    const [uri, content] = await client.openAndWait(path.join(workspace, "main.cpp"));

    // Add a line that starts typing "Poi" to trigger completion for Point.
    const newContent = content + "\nPoi";
    const lines = newContent.split("\n");
    const lastLine = lines.length - 1;

    client.change(uri, 1, newContent);

    // The completion request itself triggers compilation via ensure_compiled().
    const result = await client.completionAt(uri, lastLine, 3);
    // Completion should return results.
    expect(result).not.toBeNull();
    client.close(uri);
});

test("preamble edit then hover", async ({ client, workspace }) => {
    /// After editing the preamble (adding an #include), AST should still work.
    const [uri, content] = await client.openAndWait(path.join(workspace, "main.cpp"));

    // Verify initial state is clean.
    assertCleanCompile(client, uri);

    // Edit the preamble: add a second #include (triggers PCH rebuild).
    // Use project-local header instead of system header (<cstdio>) to avoid
    // slow PCH rebuilds on macOS CI that cause SIGPIPE timeouts.
    const newContent =
        '#include "common.h"\n#include "common.h"\n' + content.split("\n").slice(1).join("\n");
    client.change(uri, 1, newContent);

    // Trigger recompilation via hover — this will rebuild PCH with new preamble.
    await waitForRecompile(client, uri);

    // AST should still be valid — no errors.
    assertNoErrors(client, uri, "Expected no errors after preamble edit");

    // Hover should still work on a symbol.
    const result = await client.hoverAt(uri, 3, 4);
    expect(result, "Hover failed after preamble edit").not.toBeNull();
    client.close(uri);
});

test("preamble edit multiple times", async ({ client, workspace }) => {
    /// Multiple preamble edits should not break AST building.
    const [uri, content] = await client.openAndWait(path.join(workspace, "main.cpp"));

    for (let i = 0; i < 3; i++) {
        // Add progressively more includes.
        let includes = '#include "common.h"\n';
        for (let j = 0; j < i + 1; j++) {
            includes += `// edit ${j}\n`;
        }
        const newContent = includes + content.split("\n").slice(1).join("\n");

        client.change(uri, i + 1, newContent);

        await waitForRecompile(client, uri);
    }

    // After multiple edits, should still be clean.
    assertNoErrors(client, uri, "Expected no errors after multiple preamble edits");
    client.close(uri);
});
