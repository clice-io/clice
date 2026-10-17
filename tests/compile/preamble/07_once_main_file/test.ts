import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// a.h guards itself with #pragma once in its preamble; past it, a.h
// includes b.h, which includes a.h again.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.diagnostics("a.h")).toEqual([]);
});
