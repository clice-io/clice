import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Two modules import each other; a tool drops B's import of A on disk, and
// nothing is saved or edited before any tick: the next request on main.cpp
// finds what the cycle's failures rest on changed, and both build.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("before", s.errors("main.cpp"));
    t.disk.edit("cycle_b.cppm", { replace: "import CycA;\n", with: "" });
    expect(await s.errors("main.cpp")).toEqual([]);
});
