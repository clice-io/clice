import { test } from "@clice/tools/testing";

test("plans without writing", async (t) => {
    await t.cli("index --workers 2");
    await t.cli("refactor rename --symbol compute --to evaluate --dry-run");
});

test("writes the edits", async (t) => {
    await t.cli("index --workers 2");
    await t.cli("refactor rename --symbol Widget --to Gadget");
});
