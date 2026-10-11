import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --workers 2");
    await t.cli("refactor rename --spec same_name.txt --dry-run");
    await t.cli("refactor rename --spec other_name.txt --dry-run");
});
