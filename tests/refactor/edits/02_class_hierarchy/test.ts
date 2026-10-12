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
    // The template's call is left to check: the index cannot tell what it calls.
    await t.cli("refactor rename --symbol Circle::area --to size --dry-run");
});
