import { test } from "@clice/tools/testing";

test("renames a class", async (t) => {
    await t.cli("index --workers 2");
    await t.cli("refactor rename --symbol Shape --to Figure --dry-run");
});

test("a constructor names its class", async (t) => {
    await t.cli("index --workers 2");
    await t.cli("refactor rename --symbol Circle::Circle --to Disk --dry-run");
});

test("renames the overrides", async (t) => {
    await t.cli("index --workers 2");
    // The template's call goes too: its only instantiation calls Circle::area.
    await t.cli("refactor rename --symbol Circle::area --to size --dry-run");
});
