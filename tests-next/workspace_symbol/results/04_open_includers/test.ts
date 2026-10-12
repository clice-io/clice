import { test } from "@clice/tools/testing";
import { expect } from "vitest";

// A header whose only includer is open before the background index reaches
// it: the open source is indexed all the same, and the search finds the
// header's symbols whether the source is open or closed.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.indexed();
    for (const query of ["Widget", "Shape", "area"]) {
        await t.show(`open ${query}`, s.workspaceSymbols(query));
    }
    s.close("main.cpp");
    await s.indexed();
    for (const query of ["Shape", "area"]) {
        await t.show(`closed ${query}`, s.workspaceSymbols(query));
    }
});
