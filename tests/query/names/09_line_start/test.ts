import { test } from "@clice/tools/testing";

// A reference's context is its whole line, also when the name opens it.
test(async (t) => {
    await t.cli("index");
    await t.cli("query references value --include-declaration");
});

// Without a locator, every symbol, best first.
test("every symbol", async (t) => {
    await t.cli("index");
    await t.cli("query symbol-search --limit 3");
});
