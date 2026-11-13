import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

answers("compiled", async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.semanticTokens(m));
    }
});

// The tokens the index serves before the file compiles merge the meanings too.
test("index", async (t) => {
    const s = await t.start({ config: { project: { readonly: "on" } } });
    s.open("main.cpp", { pull: false });
    await s.sync();
    for (const m of t.markers()) {
        await t.show(m.name, s.semanticTokens(m));
    }
    expect((await s.counts()).compile).toBe(0);
});
