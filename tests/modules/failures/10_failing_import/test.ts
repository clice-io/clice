import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A fails because B does, and a tool changes the modules on disk: nothing is
// saved, and main.cpp asks before any tick.

test("B fixed", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("before", s.errors("main.cpp"));
    t.disk.edit("b.cppm", { replace: "return broken_in_b;", with: "return 1;" });
    expect(await s.errors("main.cpp")).toEqual([]);
});

// A drops its import of B: A builds again, though B still fails.

test("import dropped, main.cpp edited", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("before", s.errors("main.cpp"));
    t.disk.edit("a.cppm", { replace: "import B;\n", with: "" });
    t.disk.edit("a.cppm", { replace: "return b();", with: "return 1;" });
    s.edit("main.cpp", { replace: "return a();", with: "return a() + 0;" });
    expect(await s.errors("main.cpp")).toEqual([]);
});

// Nothing is edited either: the next request on main.cpp finds what A's
// failure rests on changed and compiles main.cpp again.
test("import dropped, unseen", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("before", s.errors("main.cpp"));
    t.disk.edit("a.cppm", { replace: "import B;\n", with: "" });
    t.disk.edit("a.cppm", { replace: "return b();", with: "return 1;" });
    expect(await s.errors("main.cpp")).toEqual([]);
});
