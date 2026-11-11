import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// twice<Hidden> is instantiated while main.cpp's preamble is built, never
// by the compile loading its PCH: the index beside the PCH holds what the
// instantiation resolves, so the use in extra.h answers with no background
// index at all.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    s.open("main.cpp");
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("radius", s.references(t.marker("hidden")));
});
