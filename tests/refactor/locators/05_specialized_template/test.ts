import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --workers 2");
    await t.cli("query --method references --symbol Holder");
    await t.cli("refactor rename --symbol Holder --to Box --dry-run");
    await t.cli('refactor rename --symbol "Holder<bool>" --to Box --dry-run');
});
