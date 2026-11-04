import { test } from "@clice/tools/testing";

// F12 right after opening a file, before the background index ran: the
// module interface the name comes from is indexed by the build of its PCM,
// which the importer's compile waits on. A rule keeps the importer out of
// the index, which could otherwise index it before it opens: the request
// reaches its compile.
test("importer not indexed", async (t) => {
    const s = await t.start({ config: { rules: [{ patterns: ["main.cpp"], index: false }] } });
    s.open("main.cpp");
    await t.show("before the index", s.definition(t.marker("use")));
    await s.indexed();
    await t.show("after the index", s.definition(t.marker("use")));
});

// The interface a rule keeps out of the index: the build of its PCM, which
// the importer's index waits on, indexes it all the same.
test("importer indexed first", async (t) => {
    const s = await t.start({ config: { rules: [{ patterns: ["math.cppm"], index: false }] } });
    await s.indexed();
    s.open("main.cpp");
    await t.show("definition", s.definition(t.marker("use")));
});
