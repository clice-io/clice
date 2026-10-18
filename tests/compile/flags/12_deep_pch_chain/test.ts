import { test } from "@clice/tools/testing";

// clang follows a chain of PCHs from its last one: beta.h relies on what
// alpha.pch holds, main.cpp on both. clice builds a precompiled header the
// build declares over no other, so beta.pch fails to build and main.cpp
// compiles with no PCH at all.

test("pch over a pch", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("main.cpp", s.diagnostics("main.cpp"));
    await t.show("hover", s.hover(t.marker("beta")));
    await t.show("definition", s.definition(t.marker("value")));
});

test("index", async (t) => {
    await t.cli("index");
    await t.cli("query references Alpha::a --include-declaration");
});
