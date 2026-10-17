import { test } from "@clice/tools/testing";

// p1 and p2 each declare a module named util: each main.cpp imports the
// nearest, and the import says which one it passed over.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("p1", s.diagnostics("p1/main.cpp"));
    await t.show("p2", s.diagnostics("p2/main.cpp"));
});
