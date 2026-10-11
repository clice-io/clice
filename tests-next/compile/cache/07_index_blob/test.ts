import { expect } from "vitest";
import { test, type Serve, type TestContext } from "@clice/tools/testing";

// Beside each PCH the cache keeps an index of what it precompiled, which
// navigation into the preamble's headers reads. A PCH without a sound
// index is no hit: both are built again.

const INDEX = ".clice/cache/*/pch/*.pch.idx";

async function rebuilt(t: TestContext, s: Serve, damaged: string | undefined): Promise<void> {
    await t.show("foo", s.definition(t.at("main.cpp", "|foo();")));
    await s.sync();
    expect((await s.counts()).pch).toBe(1);
    const index = t.disk.list(INDEX);
    expect(index).toHaveLength(1);
    expect(index[0] === damaged).toBe(false);
}

test("damaged index is rebuilt", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    const [index] = t.disk.list(INDEX);
    await s.offline(() => {
        t.disk.corrupt(index ?? "", "all");
    });
    expect(await s.errors("main.cpp")).toEqual([]);
    await rebuilt(t, s, index);
});

test("missing index is rebuilt", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    const [index] = t.disk.list(INDEX);
    await s.offline(() => {
        t.disk.rm(index ?? "");
    });
    expect(await s.errors("main.cpp")).toEqual([]);
    await rebuilt(t, s, index);
});
