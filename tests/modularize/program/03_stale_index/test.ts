import { test } from "@clice/tools/testing";

// A rename writes the files it edits without reindexing them: modularize
// must not wrap the index as it was.
test(async (t) => {
    await t.cli("index");
    await t.cli("refactor rename --symbol core::Text::size --to length");
    await t.cli("modularize --scope app/** --partition partition.json --out wrap");
});
