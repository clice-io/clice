import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// only.h compiles on its own, where FAST is undefined: its context indexes
// nothing in it. fast_host.cpp's context compiles all of it, and the
// answers take that from the index: the mark's section without the mark,
// the destructor's name whole.
test("the host indexed", async (t) => {
    const s = await t.start();
    await s.indexed();
    expect(await s.errors("only.h")).toEqual([]);
    await t.show("only.h", {
        tokens: s.semanticTokens("only.h"),
        outline: s.documentSymbols("only.h"),
        hover: s.hover(t.marker("fast")),
    });
});

test("the host leaves the database", async (t) => {
    // The shard of only.h outlives its last host until the index is saved:
    // no other context is left to merge.
    const s = await t.start();
    await s.indexed();
    expect(await s.errors("only.h")).toEqual([]);
    t.disk.database({});
    expect(await s.poll("cdb")).toBe(1);
    // The new command compiles only.h again; the AST answers.
    expect(await s.errors("only.h")).toEqual([]);
    await t.show("host gone", {
        tokens: s.semanticTokens("only.h"),
        hover: s.hover(t.marker("fast")),
    });
});
