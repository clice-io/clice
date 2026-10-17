import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// decls.h needs main.cpp's Handle and sits in a namespace a macro opens,
// which no brace the context counts closes: the scope stays open past
// decls.h's own text, and decls.h reports nothing.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.diagnostics("decls.h")).toEqual([]);
});
