import { test } from "@clice/tools/testing";

// main.cpp defines FAST_PATH, which the target's PCH was built without and
// its header tests. clang loads the PCH all the same, as clang's own build
// does: level() is the one without FAST_PATH. GCC would discard its .gch
// and parse the header with FAST_PATH defined.

test("unit define", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("main.cpp", s.diagnostics("main.cpp"));
    await t.show("level", s.definition(t.marker("level")));
});
