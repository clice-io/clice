import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// C99 dropped implicit declarations: a call to a function only the C
// includer declares is a missing name.
test(async (t) => {
    const s = await t.start();
    await t.show("util.h", s.diagnostics("util.h"));
    expect((await s.stats()).synthesizedContexts).toBe(1);
});
