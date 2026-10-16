import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("lint --jobs 2");
    await t.cli("lint --configuration release --jobs 2");
});
