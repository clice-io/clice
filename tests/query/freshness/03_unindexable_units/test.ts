import { test } from "@clice/tools/testing";

// A unit that fails to index has no entries: the answer lists it with the
// withheld files rather than pass silence off as completeness, and a symbol
// not found says why it may be missing. The database names a source that
// does not exist.
test(async (t) => {
    t.disk.database({ "main.cpp": [], "ghost.cpp": [] });
    await t.cli("query symbol-search ghost --fresh");
    await t.cli("query definition ghost --fresh");
});
