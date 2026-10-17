import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A's global module fragment includes leaf.h; a tool deletes it.

test("asked", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    t.disk.rm("leaf.h");
    await t.show("errors", s.errors("main.cpp"));
});

test("seen by the tick", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    t.disk.rm("leaf.h");
    expect(await s.poll("workspace")).toBe(1);
    await t.show("errors", s.errors("main.cpp"));
});
