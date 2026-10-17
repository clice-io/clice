import { test } from "@clice/tools/testing";
import { expect } from "vitest";

// Indexed before main.cpp opens: other.cpp, never opened, answers from the
// background index, main.cpp from its open buffer.
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("cache", s.workspaceSymbols("cache"));
});
