import { test } from "@clice/tools/testing";

// A name none spells exactly, case included, names no symbol: its closest
// matches are candidates. One that does is the answer, whatever differs
// from it only in case.
test(async (t) => {
    await t.cli("index");
    await t.cli("query definition Circle");
    await t.cli("query definition are");
    await t.cli("query definition width");
    await t.cli("query definition wid");
    await t.cli(["query", "definition", "are*"]);
    await t.cli(["query", "definition", "are kind:field"]);
});
