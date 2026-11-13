import { modularize, test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index");
    const run = await t.cli("modularize --scope lib/** --partition partition.json --out wrap");
    // The program as the plan builds it compiles cleanly.
    await modularize.compiles(t, run, "wrap");
});
