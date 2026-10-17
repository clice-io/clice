import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// M imports A, then B. While M's round waits for B to rebuild, A's PCM
// leaves the store and A starts rebuilding it: M reads A's build only once
// it is done. only.cpp answers once B is built, by when M has read A.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.errors("other.cpp")).toEqual([]);
    expect(await s.errors("only.cpp")).toEqual([]);
    const b = await s.park("pcm", "b.cppm", { at: "reply" });
    t.disk.edit("b.cppm", { replace: "return 2;", with: "return 3;" });
    expect(await s.poll("workspace")).toBe(1);
    const main = s.errors("main.cpp");
    await b.reached();
    const a = await s.park("pcm", "a.cppm", { at: "reply" });
    const lost = t.disk.list(".clice/cache/*/pcm/A-*.pcm");
    expect(lost).toHaveLength(1);
    t.disk.rm(lost[0] ?? "");
    s.edit("other.cpp", { replace: "return a();", with: "return a() + 0;" });
    const other = s.errors("other.cpp");
    await a.reached();
    const only = s.errors("only.cpp");
    await b.release();
    expect(await only).toEqual([]);
    await a.release();
    expect(await other).toEqual([]);
    expect(await main).toEqual([]);
});
