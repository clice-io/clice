import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The index holds a file's shard in memory only until the round that
// built it saves: then it lives on disk, and a later round writes only
// the shards it changed.

test("a round leaves no shard in memory", async (t) => {
    const s = await t.start();
    await s.indexed();
    const { indexInmemoryShards, lastSaveShards } = await s.stats();
    await t.show("after the round", { indexInmemoryShards, lastSaveShards });
});

test("a save writes the changed shard", async (t) => {
    // Open before the project starts: its first round leaves the open file
    // to the file's own compile.
    const s = await t.spawn();
    s.open("file0.cpp", { pull: false });
    await s.initialize();
    await s.diagnostics("file0.cpp");
    await s.indexed();
    t.disk.write("file2.cpp", "int func_2_renamed() { return 2; }\n");
    await s.sync({ poll: true });
    await t.show("symbols", s.workspaceSymbols("func_2"));
    const changed = await s.stats();
    // The files include nothing, so no header shard fans out, and only
    // the background index merges shards: the open file's compile does not.
    await t.show("after the change", {
        indexInmemoryShards: changed.indexInmemoryShards,
        lastSaveShards: changed.lastSaveShards,
    });
    // The open file compiles itself, so the rounds leave its disk shard
    // alone and saving the same bytes queues nothing; closing it hands the
    // file back to the background index.
    s.save("file0.cpp");
    await s.sync();
    const saved = await s.stats();
    expect(saved.indexInmemoryShards).toBe(0);
    expect(saved.indexShardContentBytes).toBe(changed.indexShardContentBytes);
    s.close("file0.cpp");
    await s.sync();
    const closed = await s.stats();
    expect(closed.indexInmemoryShards).toBe(0);
    expect(closed.indexShardContentBytes).toBeGreaterThan(saved.indexShardContentBytes);
});
