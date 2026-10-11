import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --workers 2");
    await t.cli("refactor rename --symbol compute --to int");
    // A keyword of C alone.
    await t.cli("refactor rename --symbol compute --to restrict --dry-run");
    await t.cli("refactor rename --symbol compute --to 1abc --dry-run");
    await t.cli("refactor rename --symbol compute --to a-b --dry-run");
    // Reserved for the implementation: a warning, the rename goes on.
    await t.cli("refactor rename --symbol compute --to _Compute --dry-run");
    await t.cli("refactor rename --symbol compute --to compute");
});
