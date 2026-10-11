import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const CHANGED = "export module Leaf;\n\nexport long leaf() {\n    return 100;\n}\n";

// Leaf changes while mid.cppm, its importer, stays open: Leaf compiles
// anew, and the importer's answers follow.
test("reopened", async (t) => {
    const s = await t.start();
    expect(await s.errors("mid.cppm")).toEqual([]);
    const call = t.at("mid.cppm", "return |leaf()");
    await t.show("before", s.hover(call));
    expect(await s.errors("leaf.cppm")).toEqual([]);
    s.close("leaf.cppm");
    t.disk.write("leaf.cppm", CHANGED);
    s.open("leaf.cppm");
    expect(await s.errors("leaf.cppm")).toEqual([]);
    await t.show("after", s.hover(call));
    expect(await s.errors("mid.cppm")).toEqual([]);
    await t.show("recompiled", s.hover(call));
});

test("changed on disk", async (t) => {
    const s = await t.start();
    expect(await s.errors("mid.cppm")).toEqual([]);
    const call = t.at("mid.cppm", "return |leaf()");
    t.disk.write("leaf.cppm", CHANGED);
    await s.sync({ poll: true });
    await t.show("after", s.hover(call));
    expect(await s.errors("mid.cppm")).toEqual([]);
    await t.show("recompiled", s.hover(call));
});
