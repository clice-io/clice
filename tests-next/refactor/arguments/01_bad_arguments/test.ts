import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("refactor");
    await t.cli("refactor extract");
    await t.cli("refactor rename --symbol compute");
    await t.cli("refactor rename --to evaluate");
    await t.cli('refactor rename --symbol compute --to ""');
});
