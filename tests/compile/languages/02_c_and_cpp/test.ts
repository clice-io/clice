import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

// lib.c is C, where `class` is a name; both units start with the same
// include line.

answers("hover", async (a) => {
    await a.show("c", a.hover(a.at("lib.c", "return |language")));
    await a.show("cpp", a.hover(a.at("main.cpp", "return |language")));
});

test("languages keep their own pch", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("lib.c")).toEqual([]);
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.sync();
    expect((await s.counts()).pch).toBe(2);
});
