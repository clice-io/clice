import { test } from "@clice/tools/testing";
import { expect } from "vitest";

test(async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.indexed();
    await t.show("helper_elsewhere", s.workspaceSymbols("helper_elsewhere"));
});
