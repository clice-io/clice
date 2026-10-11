import { test } from "@clice/tools/testing";
import { expect } from "vitest";

// Indexed before main.cpp opens: noise.h answers from the background index,
// main.cpp from its open buffer.
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    expect(await s.errors("main.cpp")).toEqual([]);
    for (const query of ["Flag", "N", "TT", "anonymous", "M", "local_value"]) {
        await t.show(query, s.workspaceSymbols(query));
    }
});
