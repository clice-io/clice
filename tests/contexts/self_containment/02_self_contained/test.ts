import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A header that compiles on its own borrows main.cpp's command and nothing
// of what main.cpp declares.
test("found out", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("helper.h", s.diagnostics("helper.h"));
    expect((await s.stats()).synthesizedContexts).toBe(0);
});

// A rule saying the header does not compile on its own puts it in its
// includer's context all the same.
test("declared not self-contained", async (t) => {
    const s = await t.start({
        config: { rules: [{ patterns: ["helper.h"], self_contained: false }] },
    });
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("helper.h in main.cpp", s.diagnostics("helper.h"));
    expect((await s.stats()).synthesizedContexts).toBe(1);
});
