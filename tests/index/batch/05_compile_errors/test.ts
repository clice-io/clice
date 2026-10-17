import { test } from "@clice/tools/testing";

test("text", async (t) => {
    await t.cli("index");
});

test("json", async (t) => {
    await t.cli("index --json");
});
