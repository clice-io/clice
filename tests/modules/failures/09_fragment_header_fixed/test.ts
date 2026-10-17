import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A's global module fragment includes frag.h, broken until a tool rewrites
// it on disk: nothing is saved, and no importer changes.

test("asked", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toHaveLength(1);
    t.disk.edit("frag.h", { replace: "return missing_name;", with: "return 1;" });
    expect(await s.errors("main.cpp")).toEqual([]);
});

test("seen by the tick", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toHaveLength(1);
    await s.indexed();
    t.disk.edit("frag.h", { replace: "return missing_name;", with: "return 1;" });
    expect(await s.poll("workspace")).toBe(1);
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.indexed();
    await t.show("references", s.references(t.at("a.cppm", "int |a()")));
});
