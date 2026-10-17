import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Mid imports Leaf, whose interface includes leaf.h. Nothing is open but
// Mid: each change below is made on disk.

test("saved interface reaches importers", async (t) => {
    const s = await t.start();
    expect(await s.errors("mid.cppm")).toEqual([]);
    t.disk.edit("leaf.cppm", { replace: "int leaf()", with: "int renamed_leaf()" });
    s.save("leaf.cppm");
    await t.show("mid.cppm", s.errors("mid.cppm"));
    await s.sync();
    expect((await s.counts()).files["leaf.cppm"]?.pcm).toBe(2);
    // The rebuild wrote a PCM of its own: the one before stays, for a
    // compile that may still read it, until the next build replaces it.
    expect(t.disk.list(".clice/cache/*/pcm/Leaf-*.pcm")).toHaveLength(2);
});

test("unsaved interface edit", async (t) => {
    // A checkout or a generator rewrites the interface.
    const s = await t.start();
    expect(await s.errors("mid.cppm")).toEqual([]);
    t.disk.edit("leaf.cppm", { replace: "int leaf()", with: "int renamed_leaf()" });
    await t.show("asked", s.errors("mid.cppm"));
    // The request looked at the disk itself: the background tick finds
    // nothing left to see.
    expect(await s.poll("workspace")).toBe(0);
});

test("unsaved interface header edit", async (t) => {
    const s = await t.start();
    expect(await s.errors("mid.cppm")).toEqual([]);
    t.disk.edit("leaf.h", { replace: "int helper()", with: "int helper(int scale)" });
    await t.show("asked", s.errors("mid.cppm"));
    expect(await s.poll("workspace")).toBe(0);
});
