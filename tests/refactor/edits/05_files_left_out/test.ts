import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --jobs 2");
    await t.cli("refactor rename compute evaluate");
});
