import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

answers("hover in the header", async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.hover(m));
    }
});

// Compiled alone, the header misses what main.cpp declares first: the
// server compiles it again after those declarations, and only that compile's
// diagnostics show.
test("the includer's context", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("ruler.h", s.diagnostics("ruler.h"));
    expect((await s.stats()).synthesizedContexts).toBe(1);
});

// A rule saying the header compiles on its own is taken at its word: no
// includer's context, and the errors of compiling it alone show.
test("declared self-contained", async (t) => {
    const s = await t.start({
        config: { rules: [{ patterns: ["ruler.h"], self_contained: true }] },
    });
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("ruler.h alone", s.diagnostics("ruler.h"));
    expect((await s.stats()).synthesizedContexts).toBe(0);
});
