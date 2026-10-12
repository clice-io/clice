import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The includer's context resolves lookups as the includer does: its
// __has_include probe, relative to src/, still finds the header.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("src/main.cpp")).toEqual([]);
    await t.show("src/x.h", s.diagnostics("src/x.h"));
    expect((await s.stats()).synthesizedContexts).toBe(1);
});
