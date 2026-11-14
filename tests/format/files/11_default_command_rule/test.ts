import { test } from "@clice/tools/testing";

// No database: the rule gives src/main.cpp its command, and format and the
// index take it as a unit alike.
test(async (t) => {
    await t.cli("format --check");
    await t.cli("index");
});
