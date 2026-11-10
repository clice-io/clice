import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A fails because B does; a tool drops A's import of B on disk, and nothing
// is saved or edited before any tick: the next request on main.cpp finds
// what A's failure rests on changed and compiles main.cpp again, though B
// still fails.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("before", s.errors("main.cpp"));
    t.disk.edit("a.cppm", { replace: "import B;\n", with: "" });
    t.disk.edit("a.cppm", { replace: "return b();", with: "return 1;" });
    expect(await s.errors("main.cpp")).toEqual([]);
});
