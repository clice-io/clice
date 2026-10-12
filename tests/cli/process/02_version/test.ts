import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("--version");
    await t.cli("-v");
});
