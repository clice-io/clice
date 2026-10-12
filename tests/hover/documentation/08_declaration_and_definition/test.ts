import { test } from "@clice/tools/testing";

// Clang caches the comment it finds per redeclaration chain: the card must
// not depend on which declaration is hovered first after a build. Two
// servers, one order each.
test("declaration first", async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    await s.sync();
    await t.show("at_decl", s.hover(t.marker("at_decl")));
    await t.show("at_def", s.hover(t.marker("at_def")));
});

test("definition first", async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    await s.sync();
    await t.show("at_def", s.hover(t.marker("at_def")));
    await t.show("at_decl", s.hover(t.marker("at_decl")));
});
