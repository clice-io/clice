import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

// CMake compiles a target's module units over its precompiled header too,
// with the header forced in. The header's declarations then come from the
// PCH: parsed as text, they would precede each module declaration. A PCM
// names the PCH it was built over to its importers, so the module units
// and the units importing them all read the one clice builds for the
// index, which keeps the function bodies; the header's Counter::next would
// otherwise have two definitions.
answers(
    "every unit compiles",
    {
        paths: {
            server: "clice inspect compiles one file alone, without the interfaces it imports",
        },
    },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.hover(m));
        }
    },
);

test("the units lint", async (t) => {
    await t.cli("lint");
});

test("a rule leaving the pch's builder out of the index", async (t) => {
    // The module units still read the PCH: without it they are no module
    // units at all.
    t.disk.write("clice.toml", '[[rules]]\npatterns = ["build/**"]\nindex = false\n');
    await t.cli("lint");
});

test("a header of the pch changes", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("src/main.cpp")).toEqual([]);
    t.disk.edit("include/counter.h", {
        before: "    int next()",
        insert: "    void reset() {\n        count = 0;\n    }\n\n",
    });
    // The PCM is built again over the new PCH: the old one would bring the
    // old Counter along.
    s.edit("src/main.cpp", {
        replace: "pch::Counter counter;",
        with: "pch::Counter counter;\n    counter.reset();",
    });
    expect(await s.errors("src/main.cpp")).toEqual([]);
});

test("the pch leaves the store", async (t) => {
    // As another process sharing the cache removes it: the PCMs naming it
    // are built again, over the PCH built again.
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("src/main.cpp")).toEqual([]);
    const lost = t.disk.list(".clice/cache/*/pch/*.pch");
    expect(lost).toHaveLength(1);
    t.disk.rm(lost[0] ?? "");
    s.edit("src/main.cpp", { replace: "+ twice();", with: "+ twice() + 1;" });
    expect(await s.errors("src/main.cpp")).toEqual([]);
});
