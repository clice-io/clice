import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// An edit makes a header the self-containment check found self-contained use
// what its includer declares; the save that shows it missing a name compiles
// it in the includer's context.
test("the save switches", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.errors("h.h")).toEqual([]);
    expect((await s.stats()).synthesizedContexts).toBe(0);
    s.edit("h.h", { replace: "return 1;", with: "return Host{2}.v;" });
    await t.show("edited", s.diagnostics("h.h"));
    s.save("h.h");
    await t.show("saved", s.diagnostics("h.h"));
    expect((await s.stats()).synthesizedContexts).toBe(1);
});

// Saved before the edit's compile shows what it misses, the buffer is
// judged again all the same.
test("saved before the compile", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.errors("h.h")).toEqual([]);
    s.edit("h.h", { replace: "return 1;", with: "return Host{2}.v;" });
    s.save("h.h");
    await t.show("saved", s.diagnostics("h.h"));
    expect((await s.stats()).synthesizedContexts).toBe(1);
});

// Without the includer's index, the include tree its compile took finds the
// host all the same: the header's own new text moves nothing above it.
test("the save switches without the includer's index", async (t) => {
    const s = await t.start({ config: { rules: [{ patterns: ["main.cpp"], index: false }] } });
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.errors("h.h")).toEqual([]);
    s.edit("h.h", { replace: "return 1;", with: "return Host{2}.v;" });
    await t.show("edited", s.errors("h.h"));
    s.save("h.h");
    expect(await s.errors("h.h")).toEqual([]);
    expect((await s.stats()).synthesizedContexts).toBe(1);
});

// In C a struct a prototype names first is visible in the prototype alone,
// a warning that counts as an error: the save compiles the header in the
// includer's context, which declares the struct.
test("a prototype names the includer's struct", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.c")).toEqual([]);
    expect(await s.errors("c.h")).toEqual([]);
    s.edit("c.h", { replace: "int w, int h", with: "const struct shape *s" });
    await t.show("edited", s.diagnostics("c.h"));
    s.save("c.h");
    await t.show("saved", s.diagnostics("c.h"));
    expect((await s.stats()).synthesizedContexts).toBe(1);
});
