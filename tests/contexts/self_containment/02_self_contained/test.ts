import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A header that compiles on its own borrows main.cpp's command and nothing
// of what main.cpp declares.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("helper.h", s.diagnostics("helper.h"));
    expect((await s.stats()).synthesizedContexts).toBe(0);
});
