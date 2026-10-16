import { test } from "@clice/tools/testing";

test("plans without writing", async (t) => {
    await t.cli("index --jobs 2");
    await t.cli("refactor rename compute evaluate --dry-run");
});

test("writes the edits", async (t) => {
    await t.cli("index --jobs 2");
    await t.cli("refactor rename Widget Gadget");
});
