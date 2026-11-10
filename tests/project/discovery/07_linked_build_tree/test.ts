import { test } from "@clice/tools/testing";

test("batch indexing finds a linked build tree", async (t) => {
    t.disk.symlink("project/build", "outside/build");
    await t.cli("index --workspace project --jobs 2");
});
