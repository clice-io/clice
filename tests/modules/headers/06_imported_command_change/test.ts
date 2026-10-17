import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// main.cpp's preamble imports A, which re-exports B; only B's command
// changes, so neither A's source nor its command does.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    const use = t.at("main.cpp", "return |value;");
    await t.show("before", s.hover(use));
    t.disk.database({ "b.cppm": ["-DVALUE=2"], "a.cppm": [], "main.cpp": [] });
    expect(await s.poll("cdb")).toBe(1);
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("after", s.hover(use));
    await s.sync();
    expect((await s.counts()).files["main.cpp"]?.pch).toBe(2);
});
