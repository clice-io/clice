import { test } from "@clice/tools/testing";

// Nothing in the project-wide tables names this file's static function:
// a place in the file is answered from the file's own rows.
test(async (t) => {
    await t.cli("index");
    await t.cli("query symbol-search local.cpp:1");
});
