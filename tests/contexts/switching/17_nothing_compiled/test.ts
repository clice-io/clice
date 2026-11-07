import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// only.h compiles on its own, where FAST is undefined: its context indexes
// nothing in it. fast_host.cpp's context compiles all of it, and the
// answers take that from the index: the mark's section without the mark,
// the destructor's name whole.
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    expect(await s.errors("only.h")).toEqual([]);
    await t.show("only.h", {
        tokens: s.semanticTokens("only.h"),
        outline: s.documentSymbols("only.h"),
        hover: s.hover(t.marker("fast")),
    });
});
