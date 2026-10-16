import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --jobs 2");
    await t.cli("query references Holder");
    await t.cli("refactor rename Holder Box --dry-run");
    await t.cli('refactor rename "Holder<bool>" Box --dry-run');
});
