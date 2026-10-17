import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// main.cpp imports B, which re-exports A: only A's PCM leaves the store,
// as an eviction or another process sharing the cache removes it.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    const lost = t.disk.list(".clice/cache/*/pcm/A-*.pcm");
    expect(lost).toHaveLength(1);
    t.disk.rm(lost[0] ?? "");
    s.edit("main.cpp", { replace: "return a() + b();", with: "return b() + a();" });
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.sync();
    expect((await s.counts()).files["a.cppm"]?.pcm).toBe(2);
});
