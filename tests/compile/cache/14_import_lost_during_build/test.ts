import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A's PCM leaves the store while M's build is about to read it, as an
// eviction under the cache's budget or by another server sharing it does:
// M's build fails on the missing file, which says nothing of M, so the next
// compile importing M builds A and M again.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    const build = await s.park("buildPcm", "m.cppm", { at: "worker" });
    const lost = s.errors("main.cpp");
    await build.reached();
    const pcm = t.disk.list(".clice/cache/*/pcm/A-*.pcm");
    expect(pcm).toHaveLength(1);
    t.disk.rm(pcm[0] ?? "");
    await build.release();
    await t.show("lost", lost);
    s.edit("main.cpp", { replace: "return m();", with: "return m() + 0;" });
    expect(await s.errors("main.cpp")).toEqual([]);
});
