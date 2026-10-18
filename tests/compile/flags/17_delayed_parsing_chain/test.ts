import { test } from "@clice/tools/testing";

// The build parses templates late (-fdelayed-template-parsing), the
// target's PCH keeps base_template unparsed, and main.cpp's preamble PCH,
// built over it, instantiates it. clang copies the target's record of the
// unparsed template into the preamble's PCH, and reading it back decodes
// its tokens' places in the wrong file: the compile loading the chain
// crashes.

test("late-parsed templates over the pch", async (t) => {
    const s = await t.start({
        config: { project: { enable_indexing: false } },
        crash: { request: "compile", file: "main.cpp", input: true },
    });
    await t.show("main.cpp", s.diagnostics("main.cpp"));
    await t.show("crashes", await s.crashes());
    await t.show("hover", s.hover(t.marker("base")));
});
