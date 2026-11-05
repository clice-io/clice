import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A global module fragment declaring something itself, not through a
// header, goes without a PCH: one holding only the directives above the
// declaration would put the module declaration after a declaration the
// body parsed itself.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    for (const file of ["impl.cppm", "lib.cppm", "counter.cppm", "main.cpp"]) {
        expect(await s.errors(file)).toEqual([]);
    }
    await s.sync();
    const { files } = await s.counts();
    expect([files["impl.cppm"]?.pch, files["counter.cppm"]?.pch]).toEqual([0, 0]);
});
