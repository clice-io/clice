import { test } from "@clice/tools/testing";

// core/str.h is in the database too, so the index keeps a manifest for it.
// An edit to it rebuilds the two sources that include it, not itself.
test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules impact");
});
