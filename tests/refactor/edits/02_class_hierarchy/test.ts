import { test } from "@clice/tools/testing";

test("renames a class", async (t) => {
    await t.cli("index --jobs 2");
    await t.cli("refactor rename Shape Figure --dry-run");
});

test("a constructor names its class", async (t) => {
    await t.cli("index --jobs 2");
    await t.cli("refactor rename Circle::Circle Disk --dry-run");
});

test("renames the overrides", async (t) => {
    await t.cli("index --jobs 2");
    // The template's call is left to check: the index cannot tell what it calls.
    await t.cli("refactor rename Circle::area size --dry-run");
});
