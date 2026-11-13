import { test } from "@clice/tools/testing";

test({ errors: ["Nothing to scan"] }, async (t) => {
    await t.cli("scan");
});
