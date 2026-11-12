import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// clang-tidy's findings count for nothing in the comparison, even those
// WarningsAsErrors makes errors: the header compiles in its includer's
// context, the only one where the check finds its `0`s.
test(async (t) => {
    const s = await t.start();
    await t.show("ptrs.h", s.diagnostics("ptrs.h"));
    expect((await s.stats()).synthesizedContexts).toBe(1);
});
