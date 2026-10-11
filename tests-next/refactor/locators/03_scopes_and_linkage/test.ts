import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --workers 2");
    await t.cli("refactor rename --symbol f --to h --dry-run");
    // Two units' static functions are no overload set.
    await t.cli('refactor rename --symbol "helper path:src/" --to h --dry-run');
    // One file's overloads stay a set whatever their linkage.
    await t.cli('refactor rename --symbol "mix path:src/mixed.cpp" --to h --dry-run');
    await t.cli("refactor rename --symbol S::g --to h --dry-run");
});
