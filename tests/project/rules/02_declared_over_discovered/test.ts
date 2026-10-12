import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("declared source ignores discovered database", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
});
