import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --jobs 2");
    await t.cli("refactor rename f h --dry-run");
    // Two units' static functions are no overload set.
    await t.cli('refactor rename "helper path:src/" h --dry-run');
    // One file's overloads stay a set whatever their linkage.
    await t.cli('refactor rename "mix path:src/mixed.cpp" h --dry-run');
    await t.cli("refactor rename S::g h --dry-run");
});
