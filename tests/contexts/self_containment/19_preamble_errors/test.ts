import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Compiled alone, panel.h's own lines are clean: its errors are all in
// needs.h, which it includes first and which uses the type main.cpp
// declares. Those errors, raised by the build of its preamble, count as
// its own, and the server compiles it again in main.cpp's context.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("panel.h", s.diagnostics("panel.h"));
    expect((await s.stats()).synthesizedContexts).toBe(1);
});
