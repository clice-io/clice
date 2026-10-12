import { test } from "@clice/tools/testing";

// The statistics of an index that was never built. Those of a built one
// are not pinned: the share of the bytes that file paths take depends on
// how long the copy's path is, which differs on every host.
test(async (t) => {
    await t.cli("index --stats");
    await t.cli("index --stats --top 1");
    await t.cli("index --variants");
});
