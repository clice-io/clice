import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --jobs 2");
    await t.cli("refactor rename compute int");
    // A keyword of C alone.
    await t.cli("refactor rename compute restrict --dry-run");
    await t.cli("refactor rename compute 1abc --dry-run");
    await t.cli("refactor rename compute a-b --dry-run");
    // Reserved for the implementation: a warning, the rename goes on.
    await t.cli("refactor rename compute _Compute --dry-run");
    await t.cli("refactor rename compute compute");
});
