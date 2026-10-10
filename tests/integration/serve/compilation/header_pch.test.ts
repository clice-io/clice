/// Integration tests for PCH interaction with header contexts.
///
/// Covers the unified-preamble design: the -include'd synthesized prefix is
/// baked into the PCH via the predefines buffer, clang's PPOpts validation
/// subsumes the -include on reuse (no double processing), and a header with
/// no directives of its own (bound == 0) still gets a PCH.

import type * as proto from "vscode-languageserver-protocol";
import { expect, serve } from "../../fixtures.ts";

// A bare definition in the synthesized prefix must not be processed twice
// (once in the PCH, once via -include) — that would be an error.
serve.files({
    "utils.h": "inline int next_id() { return shared_counter + 1; }\n",
    "main.cpp": 'int shared_counter = 0;\n#include "utils.h"\nint main() { return next_id(); }\n',
})("prefix not reprocessed", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.clean("utils.h");
});

// An X-macro .def file has no directives of its own (bound == 0), but the
// header context PCH must still be built to cache the prefix.
serve.files({
    "errors.def": "X(ok, 0)\nX(bad, 1)\n",
    "main.cpp":
        "#define X(name, code) inline int handle_##name() { return code; }\n" +
        '#include "errors.def"\n' +
        "#undef X\n" +
        "int main() { return handle_ok(); }\n",
})("def file builds pch", async ({ s }) => {
    await s.compiled("main.cpp");
    const before = s.workspace.pchFiles().length;

    await s.clean("errors.def");
    expect(
        s.workspace.pchFiles().length,
        "bound == 0 header context must still build a PCH for the prefix",
    ).toBe(before + 1);
});

// A self-contained header with no directives of its own has nothing to
// precompile — no PCH must be built for it.
serve.files({
    "bare.h": "inline int bare() { return 1; }\n",
    "main.cpp": '#include "bare.h"\nint main() { return bare(); }\n',
})("bare header skips pch", async ({ s }) => {
    await s.compiled("main.cpp");
    const before = s.workspace.pchFiles().length;

    await s.clean("bare.h");
    expect(
        s.workspace.pchFiles().length,
        "A self-contained header with an empty preamble must not build a PCH",
    ).toBe(before);
});

// documentLink must stay valid JSON when the PCH contributes no links (a
// preamble of only #defines) but the body has includes.
serve.files({
    "list.def": "X(alpha)\n",
    "main.cpp":
        "#define X(name) int name;\n" +
        "int before = 0;\n" +
        '#include "list.def"\n' +
        "#undef X\n" +
        "int main() { return alpha; }\n",
})("links merge empty pch", async ({ s }) => {
    await s.compiled("main.cpp");
    const links = await s.request<proto.DocumentLink[] | null>(
        "textDocument/documentLink",
        "main.cpp",
    );
    expect(links, "Expected a document link for the body include").toBeTruthy();
    expect(links!.length).toBeGreaterThan(0);
});
