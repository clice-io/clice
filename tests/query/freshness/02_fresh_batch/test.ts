import { test } from "@clice/tools/testing";

// With no server running, --fresh brings the index up to date by a batch
// run first: it builds a missing index, and reindexes what changed.
test(async (t) => {
    await t.cli("query symbol-search compute --fresh");
    t.disk.edit("main.cpp", { before: "int main()", insert: "int extra() { return 7; }\n" });
    await t.cli("query symbol-search extra --fresh");
});
