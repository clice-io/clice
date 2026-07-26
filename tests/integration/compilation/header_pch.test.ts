/// Integration tests for PCH interaction with header contexts.
///
/// Covers the unified-preamble design: the -include'd synthesized prefix is
/// baked into the PCH via the predefines buffer, clang's PPOpts validation
/// subsumes the -include on reuse (no double processing), and a header with
/// no directives of its own (bound == 0) still gets a PCH.

import { expect, test } from "../../fixtures.ts";

test("prefix not reprocessed", async ({ session }) => {
    // A bare definition in the synthesized prefix must not be processed
    // twice (once in the PCH, once via -include) — that would be an error.
    const { client, workspace } = session.tmp();
    workspace.write("utils.h", "inline int next_id() { return shared_counter + 1; }\n");
    workspace.write(
        "main.cpp",
        'int shared_counter = 0;\n#include "utils.h"\nint main() { return next_id(); }\n',
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("main.cpp");
    const [utilsUri] = await client.openAndWait("utils.h");
    client.assertCleanCompile(utilsUri);
});

test("def file builds pch", async ({ session }) => {
    // An X-macro .def file has no directives of its own (bound == 0), but
    // the header context PCH must still be built to cache the prefix.
    const { client, workspace } = session.tmp();
    workspace.write("errors.def", "X(ok, 0)\nX(bad, 1)\n");
    workspace.write(
        "main.cpp",
        "#define X(name, code) inline int handle_##name() { return code; }\n" +
            '#include "errors.def"\n' +
            "#undef X\n" +
            "int main() { return handle_ok(); }\n",
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("main.cpp");
    const before = workspace.pchFiles().length;

    const [defUri] = await client.openAndWait("errors.def");
    client.assertCleanCompile(defUri);
    expect(
        workspace.pchFiles().length,
        "bound == 0 header context must still build a PCH for the prefix",
    ).toBe(before + 1);
});

test("bare header skips pch", async ({ session }) => {
    // A self-contained header with no directives of its own has nothing to
    // precompile — no PCH must be built for it.
    const { client, workspace } = session.tmp();
    workspace.write("bare.h", "inline int bare() { return 1; }\n");
    workspace.write("main.cpp", '#include "bare.h"\nint main() { return bare(); }\n');
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("main.cpp");
    const before = workspace.pchFiles().length;

    const [bareUri] = await client.openAndWait("bare.h");
    client.assertCleanCompile(bareUri);
    expect(
        workspace.pchFiles().length,
        "A self-contained header with an empty preamble must not build a PCH",
    ).toBe(before);
});

test("links merge empty pch", async ({ session }) => {
    // documentLink must stay valid JSON when the PCH contributes no links
    // (a preamble of only #defines) but the body has includes.
    const { client, workspace } = session.tmp();
    workspace.write("list.def", "X(alpha)\n");
    workspace.write(
        "main.cpp",
        "#define X(name) int name;\n" +
            "int before = 0;\n" +
            '#include "list.def"\n' +
            "#undef X\n" +
            "int main() { return alpha; }\n",
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    const [mainUri] = await client.openAndWait("main.cpp");
    const links = await client.documentLinks(mainUri);
    expect(links, "Expected a document link for the body include").toBeTruthy();
    expect(links!.length).toBeGreaterThan(0);
});
