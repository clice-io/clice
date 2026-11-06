import { test } from "@clice/tools/testing";

// Two modules import each other: the run reports the cycle for both and
// for their importer, and indexes the rest.
test("text", async (t) => {
    await t.cli("index --jobs 2");
});

test("json", async (t) => {
    await t.cli("index --jobs 2 --json");
});
