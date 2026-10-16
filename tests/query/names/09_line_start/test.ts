import { test } from "@clice/tools/testing";

// A reference's context is its whole line, also when the name opens it.
// Without a locator, a search lists every symbol, best first.
test(async (t) => {
    await t.cli("index");
    await t.cli("query references value --include-declaration");
    await t.cli("query symbol-search --limit 3");
});
