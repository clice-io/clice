import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

answers(
    "hover",
    {
        paths: {
            server: "clice inspect compiles one file alone, without the interfaces it imports",
        },
    },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.hover(m));
        }
    },
);

test("the shared module builds once", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.sync();
    const { files } = await s.counts();
    const pcms = Object.fromEntries(Object.entries(files).map(([file, n]) => [file, n.pcm]));
    expect(pcms).toEqual({ "base.cppm": 1, "left.cppm": 1, "right.cppm": 1, "main.cpp": 0 });
});
