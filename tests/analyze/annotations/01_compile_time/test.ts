import { test } from "@clice/tools/testing";

// core/fast.h is entered by app/main.cpp alone, but every source imports
// core or a module whose interface imports it.
test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules --view impact");
    await t.cli("analyze modules --view impact --annotation compile_time.json");
});
