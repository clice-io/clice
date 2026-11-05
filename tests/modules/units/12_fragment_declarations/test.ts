import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A declaration written in a global module fragment belongs to the preamble
// with the fragment's directives: left to the body, it would put the module
// declaration after a declaration the body parsed itself. impl.cppm keeps
// its PCH; counter.cppm's fragment imports and goes without one.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    for (const file of ["impl.cppm", "lib.cppm", "counter.cppm", "main.cpp"]) {
        expect(await s.errors(file)).toEqual([]);
    }
    await s.sync();
    const { files } = await s.counts();
    expect([files["impl.cppm"]?.pch, files["counter.cppm"]?.pch]).toEqual([1, 0]);
});
