import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("modules elsewhere cost nothing", async (t) => {
    // Indexing off: indexing user.cpp looks for its imports.
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    s.edit("main.cpp", { replace: "return FORCED;", with: "return FORCED + 1;" });
    expect(await s.errors("main.cpp")).toEqual([]);
    expect((await s.stats()).importScans).toBe(0);
});
