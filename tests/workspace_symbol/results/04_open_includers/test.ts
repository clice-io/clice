import { test } from "@clice/tools/testing";
import { expect } from "vitest";

// A header whose only includer is open before the background index reaches
// it: the open source is not indexed, and the search finds only the
// header's symbols the source itself mentions until the source closes.
test(async (t) => {
    const s = await t.spawn();
    s.open("main.cpp", { pull: false });
    await s.initialize();
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
