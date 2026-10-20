import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// main.cpp's command forces in cmake_pch.hxx, which includes config.h and
// then widget.h, which uses what config.h declares without including it.
// Without the forced header, which would enter it before its own text,
// widget.h does not compile alone: the server compiles it again after what
// main.cpp includes before it, as any header that needs its includer.
test("the includer's context", async (t) => {
    const s = await t.start();
    await t.show("widget.h", s.diagnostics("widget.h"));
    for (const m of t.markers()) {
        await t.show(m.name, s.hover(m));
    }
    expect((await s.stats()).synthesizedContexts).toBe(1);
});
