import { test } from "@clice/tools/testing";

// sub/ is a folder of its own but no project: the root's project serves it.
// A clice.toml created there makes it one, through a restart.
test(async (t) => {
    const s = await t.start({ folders: [".", "sub"] });
    await s.sync();
    t.disk.write("sub/clice.toml", "[project]\nenable_indexing = false\n");
    await s.poll("workspace");
    await t.show("asked", await s.restarted());
});
