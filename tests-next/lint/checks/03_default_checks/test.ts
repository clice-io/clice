import { test } from "@clice/tools/testing";

test("without a configuration", async (t) => {
    t.disk.rm(".clang-tidy");
    await t.cli("lint --workers 2");
});

test("checks without -*", async (t) => {
    await t.cli("lint --workers 2");
});
