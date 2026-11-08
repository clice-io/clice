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
    // The template's call follows its one instantiation, `measure(Circle(1.0))`.
    await t.cli("refactor rename Circle::area size --dry-run");
});
