import { modularize, test } from "@clice/tools/testing";

// Two headers whose names are letters clang takes in an identifier but no
// ASCII ones: each partition keeps its file's name.
test(async (t) => {
    await t.cli("index");
    const run = await t.cli("modularize --scope app/** --partition partition.json --out wrap");
    // The program as the plan builds it compiles cleanly.
    await modularize.compiles(t, run, "wrap");
});
