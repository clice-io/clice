import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The header refuses to compile unless its includer defines VIA_HOST first.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("guard.h", s.diagnostics("guard.h"));
});
