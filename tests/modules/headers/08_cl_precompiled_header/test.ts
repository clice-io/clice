import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

// MSVC's form of a target's precompiled header: the writer creates it with
// /Yc, every unit reads it with /Yu, /Fp naming it and /FI forcing the
// header in. A module unit's declarations then follow the PCH, and its
// `module;` stays the first thing it parses.
answers(
    "every unit compiles",
    {
        paths: {
            server: "clice inspect compiles one file alone, without the interfaces it imports",
        },
    },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.hover(m));
        }
    },
);

test("no errors", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("src/counting.cppm")).toEqual([]);
    expect(await s.errors("src/main.cpp")).toEqual([]);
});
