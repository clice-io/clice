import { test } from "@clice/tools/testing";

// F12 right after opening a file, before the background index has read the
// module interface the name comes from: the interface's index is held back
// so the request lands before it, as it does when a user is quick. A rule
// keeps the importer out of the index, which could otherwise index it
// before it opens: the request reaches its compile.
test("importer not indexed", async (t) => {
    const s = await t.start({ config: { rules: [{ patterns: ["main.cpp"], index: false }] } });
    const held = await s.park("index", "math.cppm", { at: "reply" });
    s.open("main.cpp");
    await t.show("before the index", s.definition(t.marker("use")));
    await held.release();
    await s.indexed();
    await t.show("after the index", s.definition(t.marker("use")));
});

// The importer indexed before the interface, which a rule keeps out of the
// index for good: the importer's index names the symbol and places it
// nowhere.
test("importer indexed first", async (t) => {
    const s = await t.start({ config: { rules: [{ patterns: ["math.cppm"], index: false }] } });
    await s.indexed();
    s.open("main.cpp");
    await t.show("definition", s.definition(t.marker("use")));
});
