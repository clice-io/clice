import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A syntax error is the header's own, not a sign it needs its includer.
test(async (t) => {
    const s = await t.start();
    await t.show("typo.h", s.diagnostics("typo.h"));
    expect((await s.stats()).synthesizedContexts).toBe(0);
});
