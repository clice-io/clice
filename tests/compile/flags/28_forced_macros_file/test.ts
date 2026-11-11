import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// main.cpp's command forces in prefix.h and, with -imacros, config.h, whose
// macros clang reads before any -include wherever the command writes it:
// prefix.h uses CONFIG_VALUE. config.h includes inc/board.h through
// main.cpp's -Iinc. Each header compiles where main.cpp's compile enters
// it: config.h alone, which would otherwise enter itself and skip its
// guarded body, board.h after what config.h holds before it, and prefix.h
// after config.h.
test("headers of a macros file", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    for (const file of ["config.h", "inc/board.h", "prefix.h"]) {
        await t.show(file, s.diagnostics(file));
    }
    for (const m of t.markers()) {
        await t.show(m.name, s.definition(m));
    }
});
