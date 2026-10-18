import { test } from "@clice/tools/testing";

// main.cpp's preamble is built over the target's PCH and instantiates
// twice() from it, then main.cpp instantiates thrice(). Had the PCHs kept
// clang's deferred template bodies, the preamble would copy the deferred
// bodies of the PCH below it, and clang crashes reading them back.

test("templates instantiate across the pch", async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("src/main.cpp"));
    await t.show("call", s.hover(t.marker("call")));
});

test("the pchs index template bodies", async (t) => {
    // Nothing is indexed: the uses of one() in the headers come from the
    // indexes of the target's PCH and of main.cpp's preamble, inside
    // templates nothing instantiates.
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await s.diagnostics("src/main.cpp");
    await t.show("one", s.references(t.marker("use")));
});
