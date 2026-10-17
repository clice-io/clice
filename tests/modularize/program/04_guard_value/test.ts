import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index");
    await t.cli("modularize --scope app/** --partition partition.json --out wrap");
});
