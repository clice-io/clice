import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// p1 and p2 each declare a module named util: each main.cpp imports the
// nearest, and the import says which one it passed over. main.cpp, as near
// to both, imports the first by path.
test("nearest", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("p1", s.diagnostics("p1/main.cpp"));
    await t.show("p2", s.diagnostics("p2/main.cpp"));
    await t.show("root", s.diagnostics("main.cpp"));
});

// p2's util declares another name until a tool renames it.
test("declared on disk", async (t) => {
    t.disk.edit("p2/util.cppm", { replace: "export module util;", with: "export module other;" });
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("before", s.diagnostics("p2/main.cpp"));
    t.disk.edit("p2/util.cppm", { replace: "export module other;", with: "export module util;" });
    expect(await s.poll("workspace")).toBe(1);
    await t.show("after", s.diagnostics("p2/main.cpp"));
});

// p2's util is missing from the database until it is regenerated.
test("added to the database", async (t) => {
    t.disk.database({ "p1/util.cppm": [], "p1/main.cpp": [], "p2/main.cpp": [], "main.cpp": [] });
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("before", s.diagnostics("p2/main.cpp"));
    t.disk.database({
        "p1/util.cppm": [],
        "p1/main.cpp": [],
        "p2/util.cppm": [],
        "p2/main.cpp": [],
        "main.cpp": [],
    });
    expect(await s.poll("cdb")).toBe(1);
    await t.show("after", s.diagnostics("p2/main.cpp"));
});

// A tool renames p2's module: p1/main.cpp no longer has two to choose from.
test("undeclared on disk", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("before", s.diagnostics("p1/main.cpp"));
    t.disk.edit("p2/util.cppm", { replace: "export module util;", with: "export module other;" });
    expect(await s.poll("workspace")).toBe(1);
    expect(await s.diagnostics("p1/main.cpp")).toEqual([]);
});

// A tool deletes p2's util.
test("removed from disk", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("before", s.diagnostics("p1/main.cpp"));
    t.disk.rm("p2/util.cppm");
    expect(await s.poll("workspace")).toBe(1);
    expect(await s.diagnostics("p1/main.cpp")).toEqual([]);
});
