import { test } from "@clice/tools/testing";

// What the persisted index records about a symbol, a file and a
// translation unit, for a maintainer looking into an answer.
test("records", async (t) => {
    await t.cli("index");
    const shown = await t.cli("index symbol geo::area");
    const id = /^symbol (#[0-9a-f]+)/.exec(shown.stdout)?.[1] ?? "none";
    await t.cli(["index", "symbol", id]);
    await t.cli("index symbol area");
    await t.cli("index symbol geo.h:4");
    await t.cli("index file geo.h");
    await t.cli("index unit main.cpp");
});

test("names nothing recorded", async (t) => {
    await t.cli("index");
    await t.cli("index symbol nowhere");
    await t.cli("index file gone.h");
    await t.cli("index unit geo.h");
});
