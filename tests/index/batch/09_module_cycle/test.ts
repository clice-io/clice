import { test } from "@clice/tools/testing";

// Two modules import each other: the runs report the cycle for both and
// for their importer, as the editor does, and index the rest.
test("text", async (t) => {
    await t.cli("index --jobs 2");
});

test("json", async (t) => {
    await t.cli("index --jobs 2 --json");
});

test("lint", async (t) => {
    await t.cli("lint --jobs 2");
});
