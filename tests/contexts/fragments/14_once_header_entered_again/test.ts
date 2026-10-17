import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// value.h guards itself with #pragma once and needs main.cpp's Value;
// past its include, main.cpp includes wrapper.h, which includes it again.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.diagnostics("value.h")).toEqual([]);
});
