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
    const s = await t.start();
    await s.indexed();
    // Once indexed, an open file compiles itself and the rounds leave it
    // alone.
    await s.diagnostics("file0.cpp");
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
    // Saving the open file with the bytes its shard holds queues nothing,
    // and neither does closing it.
    s.save("file0.cpp");
    await s.sync();
    const saved = await s.stats();
    expect(saved.indexInmemoryShards).toBe(0);
    expect(saved.indexShardContentBytes).toBe(changed.indexShardContentBytes);
    s.close("file0.cpp");
    await s.sync();
    const closed = await s.stats();
    expect(closed.indexInmemoryShards).toBe(0);
    expect(closed.indexShardContentBytes).toBe(saved.indexShardContentBytes);
});
