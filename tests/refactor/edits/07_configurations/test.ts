import { test } from "@clice/tools/testing";

test("each configuration's index", async (t) => {
    await t.cli("index --workers 2");
    await t.cli("index --configuration release --workers 2");
    // The other configuration's branch is inactive: left alone, and noted.
    await t.cli("refactor rename --symbol compute --to evaluate --dry-run");
    await t.cli("refactor rename --symbol compute --to evaluate --configuration release --dry-run");
});

test("an unknown configuration", async (t) => {
    await t.cli("index --workers 2");
    await t.cli("refactor rename --symbol compute --to evaluate --configuration nope --dry-run");
});
