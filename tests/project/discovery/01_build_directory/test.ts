import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("a database under build/ is found", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
});
