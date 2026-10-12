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
        await a.show("use", a.hover(a.marker("use")));
    },
);

test("the importer builds the chain once", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.sync();
    const { files } = await s.counts();
    const pcms = Object.fromEntries(Object.entries(files).map(([file, n]) => [file, n.pcm]));
    expect(pcms).toEqual({
        "m1.cppm": 1,
        "m2.cppm": 1,
        "m3.cppm": 1,
        "m4.cppm": 1,
        "m5.cppm": 1,
        "main.cpp": 0,
    });
});
