import { test } from "@clice/tools/testing";

test({ errors: ["No index cache at"] }, async (t) => {
    await t.cli("analyze modules");
});
