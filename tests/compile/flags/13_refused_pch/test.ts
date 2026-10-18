import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The units' flags conflict with those the target's PCH is built under:
// main.cpp defines LEVEL otherwise, other.cpp takes another standard.
// clang refuses the PCH and they compile without it, so level() is the one
// their own flags select.

test("refused pch", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    for (const file of ["main.cpp", "other.cpp"]) {
        expect(await s.errors(file)).toEqual([]);
    }
    await t.show("main", s.definition(t.marker("main")));
    await t.show("other", s.definition(t.marker("other")));
});
