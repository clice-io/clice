import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("use", s.hover(t.at("main.cpp", "return |speed()")));
    await s.sync();
    const { files } = await s.counts();
    expect([files["fast.cppm"]?.pcm, files["slow.cppm"]?.pcm]).toEqual([1, undefined]);
});
