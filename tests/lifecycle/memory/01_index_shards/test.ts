import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The index holds a file's shard in memory only until the pass that
// built it saves: then it lives on disk, and a later pass writes only
// the shards it changed.

test("a pass leaves no shard in memory", async (t) => {
    const s = await t.start();
    await s.indexed();
    const { indexInmemoryShards, lastSaveShards } = await s.stats();
    await t.show("after the pass", { indexInmemoryShards, lastSaveShards });
});

test("a save writes the changed shard", async (t) => {
    // Open before the project starts: its first pass indexes the open file
    // like the closed ones, though the file's own compile parses every
    // function body too.
    const s = await t.spawn({ config: { project: { skip_header_bodies: false } } });
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
    // The open file's compile serves its buffer alone: the passes indexed
    // the file already, so saving the same bytes queues nothing, and
    // closing it changes nothing.
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
