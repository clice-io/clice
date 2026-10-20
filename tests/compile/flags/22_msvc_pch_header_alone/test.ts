import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

// main.cpp's command forces in cmake_pch.hxx with /FI, which includes
// shapes.h and, through it, point.h. Opened, each header borrows main.cpp's
// command and compiles alone, without the forced header that would enter
// it before its own text.

answers("hover", async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.hover(m));
    }
});

test("the headers compile alone", async (t) => {
    const s = await t.start();
    await t.show("shapes.h", s.diagnostics("shapes.h"));
    await t.show("point.h", s.diagnostics("point.h"));
    expect((await s.stats()).synthesizedContexts).toBe(0);
});
