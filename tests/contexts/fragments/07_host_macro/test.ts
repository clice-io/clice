import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The macro main.cpp defines before the include resolves to that
// definition.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.indexed();
    expect(await s.errors("list.def")).toEqual([]);
    await t.show("definition", s.definition(t.at("list.def", "|ITEM(alpha)")));
});
