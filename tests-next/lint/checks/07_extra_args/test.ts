import { test } from "@clice/tools/testing";

// The configuration's ExtraArgs reach the compile: the code with the
// finding exists only under the define they add.

test(async (t) => {
    await t.cli("lint --workers 2");
});
