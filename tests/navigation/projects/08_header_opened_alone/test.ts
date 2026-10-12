import { test } from "@clice/tools/testing";

// A header opened before any source including it: the index answers for
// the sources around it, and without one the header knows only itself.
test("indexed", async (t) => {
    const s = await t.start();
    await s.indexed();
    s.open("shared.h");
    const m = t.marker("decl");
    await t.show("decl", {
        definition: s.definition(m),
        references: s.references(m),
        incoming_calls: s.incomingCalls(m),
    });
});

test("unindexed", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    s.open("shared.h");
    const m = t.marker("decl");
    await t.show("decl", {
        definition: s.definition(m),
        references: s.references(m),
        incoming_calls: s.incomingCalls(m),
    });
});
