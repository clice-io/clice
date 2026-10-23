import { test } from "@clice/tools/testing";

// operator== is used only in same()'s body, which the editor's compile of
// main.cpp skips: that compile cannot tell what is unused and reports
// neither function. The background index's compile can: once it has run,
// forgotten() alone shows unused.
test("editor", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});

test("indexed", async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
