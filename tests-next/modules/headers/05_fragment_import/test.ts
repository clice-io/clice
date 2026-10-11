import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A PCH would lose the imports of a module unit's global module fragment,
// so m.cppm compiles without one; n.cppm's fragment only includes, and
// keeps its PCH.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("m.cppm")).toEqual([]);
    expect(await s.errors("n.cppm")).toEqual([]);
    await s.sync();
    const { files } = await s.counts();
    expect([files["m.cppm"]?.pch, files["n.cppm"]?.pch]).toEqual([0, 1]);
});
