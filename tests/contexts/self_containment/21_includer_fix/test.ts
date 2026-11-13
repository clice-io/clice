import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The comparison's verdict rests on what both its compiles read: main.cpp
// declares Host only after h.h, which fixes nothing, so h.h compiles alone;
// once main.cpp declares it first, open h.h compiles again, compares again
// and takes main.cpp's context.
test(async (t) => {
    const s = await t.start();
    await t.show("tied", s.diagnostics("h.h"));
    expect((await s.stats()).synthesizedContexts).toBe(0);
    const written = t.disk.mtime("main.cpp");
    t.disk.edit("main.cpp", {
        replace: '#include "h.h"\nstruct Host {};',
        with: 'struct Host {};\n#include "h.h"',
    });
    t.disk.touch("main.cpp", new Date(written.getTime() + 10_000));
    await s.sync({ poll: true });
    await t.show("fixed", s.diagnostics("h.h"));
    expect((await s.stats()).synthesizedContexts).toBe(1);
});
