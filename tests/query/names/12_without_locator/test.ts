import { test } from "@clice/tools/testing";

// Without a locator, a search lists every symbol, best first.
test(async (t) => {
    await t.cli("index");
    await t.cli("query symbol-search --limit 3");
});
