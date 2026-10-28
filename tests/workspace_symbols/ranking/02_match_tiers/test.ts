import { test } from "@clice/tools/testing";
import { expect } from "vitest";

// The answer renders sorted by place; the reply's own order, which the
// ranking decides, shows apart.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    const found = s.workspaceSymbols("foo");
    await t.show("foo", found);
    await t.show(
        "order",
        (await found).map((symbol) => symbol.name),
    );
});
