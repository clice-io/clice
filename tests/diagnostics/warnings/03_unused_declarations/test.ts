import { test } from "@clice/tools/testing";

// operator== and Point::equals are used only in the bodies of compare.h's
// templates, which the editor's compile of main.cpp skips: that compile
// cannot tell what is unused and shows none of the warnings. The background
// index's compile can: once it has run, the forgotten functions alone show
// unused, but for the one a NOLINT comment silences, as clang-tidy does in
// the editor.
test("editor", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});

test("indexed", async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
