import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("lint --workers 2");
    await t.cli("lint --configuration release --workers 2");
});
