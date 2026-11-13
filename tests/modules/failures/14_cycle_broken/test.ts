import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Two modules import each other; once B stops importing A, both build and
// their importer compiles cleanly.

test("saved", async (t) => {
    const s = await t.start();
    await t.show("before", s.errors("main.cpp"));
    t.disk.edit("cycle_b.cppm", { replace: "import CycA;\n", with: "" });
    s.save("cycle_b.cppm");
    expect(await s.errors("cycle_b.cppm")).toEqual([]);
    expect(await s.errors("cycle_a.cppm")).toEqual([]);
    expect(await s.errors("main.cpp")).toEqual([]);
});

// A tool drops B's import of A on disk, and nothing is saved or edited
// before any tick: the next request on main.cpp finds what the cycle's
// failures rest on changed.
test("unseen", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("before", s.errors("main.cpp"));
    t.disk.edit("cycle_b.cppm", { replace: "import CycA;\n", with: "" });
    expect(await s.errors("main.cpp")).toEqual([]);
});
