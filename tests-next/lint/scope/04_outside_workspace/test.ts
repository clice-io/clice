import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("lint --workspace project --workers 2");
});
