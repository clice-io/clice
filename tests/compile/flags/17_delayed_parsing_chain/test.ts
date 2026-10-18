import { test } from "@clice/tools/testing";

// The build parses templates late (-fdelayed-template-parsing), the
// target's PCH keeps base_template unparsed, and main.cpp's preamble PCH,
// built over it, instantiates it. clang would copy the target's record of
// the unparsed template into the preamble's PCH and crash reading the copy
// back; the preamble's PCH stores only its own records.

test("late-parsed templates over the pch", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("main.cpp", s.diagnostics("main.cpp"));
    await t.show("hover", s.hover(t.marker("base")));
});
