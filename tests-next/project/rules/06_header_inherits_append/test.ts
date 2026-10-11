import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("header inherits host edits", async (t) => {
    const s = await t.start();
    expect(await s.errors("include/x.h")).toEqual([]);
});
