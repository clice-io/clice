import { answers, test } from "@clice/tools/testing";

// The target's flags force global.h in, and counter.h, the target's
// precompiled header, needs what it defines: the driver puts the forced
// include of the flags before cmake_pch.hxx, which CMake passes through
// -Xclang. The PCH clice builds from cmake_pch.hxx.cxx reads them in that
// order, and so does each unit over it, the PCM of the module unit, and
// counter.h compiled alone.

answers("hover", async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.hover(m));
    }
});

test("the units lint", async (t) => {
    await t.cli("lint");
});

test("the index", async (t) => {
    await t.cli("index");
    await t.cli(`query references ${t.place(t.marker("plain"))} --include-declaration`);
});
