import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// main.cpp's command forces in prefix.h, which defines READY and then
// includes ready.h; no unit includes ready.h itself. later.h is main.cpp's
// own include, and prefix.h's only in a branch the compile does not take.
// Each header compiles where main.cpp's compile enters it: ready.h after
// what prefix.h holds before it, later.h after prefix.h whole, AFTER
// included, and prefix.h alone, which would otherwise enter itself.
test("headers of a forced include", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    for (const file of ["ready.h", "later.h", "prefix.h"]) {
        await t.show(file, s.diagnostics(file));
    }
    for (const m of t.markers()) {
        await t.show(m.name, s.definition(m));
    }
});
