import { test } from "@clice/tools/testing";
import { expect } from "vitest";

// A function renamed in the open buffer is found by its new name at once,
// and its old name no longer is.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.indexed();
    s.edit("main.cpp", { replace: "stale_name", with: "fresh_name" });
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("fresh_name", s.workspaceSymbols("fresh_name"));
    await t.show("stale_name", s.workspaceSymbols("stale_name"));
});
