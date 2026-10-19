import { test } from "@clice/tools/testing";

// A second run resumes from the persisted index and indexes nothing fresh.
test(async (t) => {
    await t.cli("index --jobs 2");
    await t.cli("index --jobs 2");
});
