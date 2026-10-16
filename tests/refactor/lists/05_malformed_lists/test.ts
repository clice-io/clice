import { test } from "@clice/tools/testing";

test(async (t) => {
    for (const list of ["no_arrow", "no_rename", "no_locator", "no_name", "bad_id", "missing"]) {
        await t.cli(`refactor rename --spec ${list}.txt`);
    }
    await t.cli("refactor rename compute evaluate --spec -");
});
