import { test } from "@clice/tools/testing";

// The commands target MSVC before C++20, where clang defers template
// bodies by default; clice parses them unless a command asks. main.cpp's
// preamble is built over the target's PCH and instantiates twice() from it,
// then main.cpp instantiates thrice(). A preamble storing again the
// deferred bodies it read from the PCH crashes the parse reading it.

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

test("deferral the command asks for", async (t) => {
    // A rule asks every compile to defer template bodies, the target's PCH
    // included. The preamble still reads the PCH's deferred bodies when it
    // instantiates twice(), but no longer stores them again. Deferred bodies
    // stay blind: neither use of one() in an uninstantiated template is
    // found.
    const s = await t.start({
        config: {
            project: { enable_indexing: false },
            rules: [{ patterns: ["**"], append: ["-fdelayed-template-parsing"] }],
        },
    });
    await t.show("main.cpp", s.diagnostics("src/main.cpp"));
    await t.show("call", s.hover(t.marker("call")));
    await t.show("one", s.references(t.marker("use")));
});
