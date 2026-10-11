import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The namespace user.h sees through its own include gains members from
// what main.cpp includes first: missing context, not errors of the header.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("user.h", s.diagnostics("user.h"));
    expect((await s.stats()).synthesizedContexts).toBe(1);
});
