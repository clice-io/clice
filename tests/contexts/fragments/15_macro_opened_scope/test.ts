import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// decls.h needs main.cpp's Handle and sits in a namespace a macro opens,
// which no brace the context counts closes: the scope stays open past
// decls.h's own text.
test(async (t) => {
    const s = await t.start({ lastRule: { background_index: false } });
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("decls.h", s.diagnostics("decls.h"));
});
