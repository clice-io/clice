import { test } from "@clice/tools/testing";

// operator== and Point::equals are used only in the bodies of compare.h's
// templates: the forgotten functions alone show unused, but for the one a
// NOLINT comment silences, as clang-tidy does in the editor, with the
// background index or without.
test("editor", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});

test("indexed", async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
