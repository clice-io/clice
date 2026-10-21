import { test } from "@clice/tools/testing";

// A workspace with no compilation database: the run fails, saying so,
// unless files are named.
test(async (t) => {
    await t.cli("format");
    await t.cli("format main.cpp");
});
