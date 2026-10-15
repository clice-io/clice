import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("scan");
    await t.cli("scan --json");
});
