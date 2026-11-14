import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("analyze modules");
});
