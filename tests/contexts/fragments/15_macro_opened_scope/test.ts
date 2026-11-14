import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// decls.h needs main.cpp's Handle and sits in a namespace a macro opens,
// which no brace the context counts closes: the scope stays open past
// decls.h's own text.
test("not indexed", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("decls.h", s.diagnostics("decls.h"));
});

// Indexed, the context comes from main.cpp's stored include tree, which
// keeps the braces the macros opened too.
test("indexed", async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("decls.h", s.diagnostics("decls.h"));
});
