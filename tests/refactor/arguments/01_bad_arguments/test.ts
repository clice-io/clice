import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("refactor");
    await t.cli("refactor extract");
    await t.cli("refactor rename compute");
    await t.cli("refactor rename");
    await t.cli('refactor rename compute ""');
});
