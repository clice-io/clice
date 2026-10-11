import { test } from "@clice/tools/testing";
import { expect } from "vitest";

// Indexed before main.cpp opens, so the header's symbols come from the
// background index (results/04_open_includers is the other order).
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    expect(await s.errors("main.cpp")).toEqual([]);
    for (const query of ["Size", "Handle", "Ptr", "id_type", "di", "Hidden"]) {
        await t.show(query, s.workspaceSymbols(query));
    }
});
