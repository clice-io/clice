/// Integration tests for PCH interaction with header contexts.
///
/// Covers the unified-preamble design: the -include'd synthesized prefix is
/// baked into the PCH via the predefines buffer, clang's PPOpts validation
/// subsumes the -include on reuse (no double processing), and a header with
/// no directives of its own (bound == 0) still gets a PCH.

import * as fs from "node:fs";
import * as path from "node:path";
import { assertCleanCompile } from "../../tools/checks.ts";
import { writeCdb } from "../../tools/compile_commands.ts";
import { expect, test } from "../../tools/fixtures.ts";
import { listPchFiles } from "../../tools/workspace.ts";

test("prefix not reprocessed", async ({ session }) => {
    /// A bare definition in the synthesized prefix must not be processed
    /// twice (once in the PCH, once via -include) — that would be an error.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(
        path.join(workspace, "utils.h"),
        "inline int next_id() { return shared_counter + 1; }\n",
    );
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        'int shared_counter = 0;\n#include "utils.h"\nint main() { return next_id(); }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait(path.join(workspace, "main.cpp"));
    const [utilsUri] = await client.openAndWait(path.join(workspace, "utils.h"));
    assertCleanCompile(client, utilsUri);
});

test("def file builds pch", async ({ session }) => {
    /// An X-macro .def file has no directives of its own (bound == 0), but
    /// the header context PCH must still be built to cache the prefix.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "errors.def"), "X(ok, 0)\nX(bad, 1)\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        "#define X(name, code) inline int handle_##name() { return code; }\n" +
            '#include "errors.def"\n' +
            "#undef X\n" +
            "int main() { return handle_ok(); }\n",
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait(path.join(workspace, "main.cpp"));
    const before = listPchFiles(workspace).length;

    const [defUri] = await client.openAndWait(path.join(workspace, "errors.def"));
    assertCleanCompile(client, defUri);
    expect(
        listPchFiles(workspace).length,
        "bound == 0 header context must still build a PCH for the prefix",
    ).toBe(before + 1);
});

test("bare header skips pch", async ({ session }) => {
    /// A self-contained header with no directives of its own has nothing to
    /// precompile — no PCH must be built for it.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "bare.h"), "inline int bare() { return 1; }\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "bare.h"\nint main() { return bare(); }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait(path.join(workspace, "main.cpp"));
    const before = listPchFiles(workspace).length;

    const [bareUri] = await client.openAndWait(path.join(workspace, "bare.h"));
    assertCleanCompile(client, bareUri);
    expect(
        listPchFiles(workspace).length,
        "A self-contained header with an empty preamble must not build a PCH",
    ).toBe(before);
});

test("links merge empty pch", async ({ session }) => {
    /// documentLink must stay valid JSON when the PCH contributes no links
    /// (a preamble of only #defines) but the body has includes.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "list.def"), "X(alpha)\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        "#define X(name) int name;\n" +
            "int before = 0;\n" +
            '#include "list.def"\n' +
            "#undef X\n" +
            "int main() { return alpha; }\n",
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [mainUri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    const links = await client.documentLinks(mainUri);
    expect(links, "Expected a document link for the body include").toBeTruthy();
    expect(links!.length).toBeGreaterThan(0);
});
