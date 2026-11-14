import { test } from "@clice/tools/testing";

// Each configuration has an index of its own: a query reads the one it
// names, else the default's.
test(async (t) => {
    await t.cli("index --configuration release");
    await t.cli("query symbol-search release_only --configuration release");
    await t.cli("query symbol-search debug_only");
    await t.cli("index");
    await t.cli("query symbol-search debug_only");
    await t.cli("query symbol-search debug_only --configuration nope");
});
