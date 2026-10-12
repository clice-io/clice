import { test } from "@clice/tools/testing";

// The statistics of an index not built yet; a second run that resumes from
// the persisted index, indexing nothing fresh again; and what the persisted
// index records. The statistics of a built index measure stored paths, so
// they read differently wherever the copy lies.

test(async (t) => {
    await t.cli("index --stats");
    await t.cli("index --workers 2");
    await t.cli("index --workers 2");
    await t.cli("index --show-symbol add");
});
