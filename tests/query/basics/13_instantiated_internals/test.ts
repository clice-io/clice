import { test } from "@clice/tools/testing";

// main.cpp instantiates box.h's template with a type of its anonymous
// namespace and with a function's own class: the read in box.h is reached
// from either member, and names both.
test("closed sources", async (t) => {
    await t.cli("index");
    await t.cli("query references main.cpp:5:12 --include-declaration");
    await t.cli("query references main.cpp:15:16 --include-declaration");
    await t.cli("query references box.h:5:22 --include-declaration");
});

// With main.cpp closed, the stored records answer the open header.
test("open header", async (t) => {
    const s = await t.start();
    await s.indexed();
    s.open("box.h");
    await s.sync();
    await t.show("read", s.references(t.at("box.h", "shape.|radius")));
});
