import { test } from "@clice/tools/testing";

// A unit that fails to index has no rows: the answer lists it with the
// withheld files rather than pass silence off as completeness. The
// database names a source that does not exist.
test(async (t) => {
    t.disk.database({ "main.cpp": [], "ghost.cpp": [] });
    await t.cli("query symbol-search ghost --fresh");
});
