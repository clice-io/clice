import { test } from "@clice/tools/testing";

// a.cpp's command is one clang cannot run. Unindexed, or indexed under its
// earlier command, a.cpp is owed a reindex when the background index starts
// after the lint pass: the lint pass fails it, the background index retries
// it and fails it again. It is one unit either way.
test("fresh cache", async (t) => {
    t.disk.database({ "a.cpp": ["--target=nonsense-arch-xyz"], "b.cpp": [] });
    await t.cli("lint --index --jobs 2");
});

test("indexed before", async (t) => {
    await t.cli("index --jobs 2");
    t.disk.database({ "a.cpp": ["--target=nonsense-arch-xyz"], "b.cpp": [] });
    await t.cli("lint --index --jobs 2");
});
