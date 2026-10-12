import { test } from "@clice/tools/testing";
import { expect } from "vitest";

// The definitions in matrix.tpp are known from main.cpp, the only source
// including the pair, through the background index: whether it is closed,
// or open before the index reached it.
test("indexed", async (t) => {
    const s = await t.start();
    await s.indexed();
    s.open("matrix.h");
    await t.show("matrix.h", s.counterparts("matrix.h"));
    await t.show("matrix.tpp", s.counterparts("matrix.tpp"));
});

test("opened first", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.indexed();
    await t.show("matrix.h", s.counterparts("matrix.h"));
});
