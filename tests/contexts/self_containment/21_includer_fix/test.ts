import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The comparison's verdict rests on what both its compiles read: main.cpp's
// own error ties it, so h.h compiles alone; once main.cpp is fixed, h.h's
// next compile compares again and takes main.cpp's context.
test(async (t) => {
    const s = await t.start();
    await t.show("tied", s.diagnostics("h.h"));
    expect((await s.stats()).synthesizedContexts).toBe(0);
    const written = t.disk.mtime("main.cpp");
    t.disk.edit("main.cpp", { replace: "int broken = ;", with: "int broken = 0;" });
    t.disk.touch("main.cpp", new Date(written.getTime() + 10_000));
    s.close("h.h");
    await t.show("fixed", s.diagnostics("h.h"));
    expect((await s.stats()).synthesizedContexts).toBe(1);
});
