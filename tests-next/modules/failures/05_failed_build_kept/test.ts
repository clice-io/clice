import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A module whose build failed is not built again for its importer's edits:
// the same inputs fail again. Saving a fix is a new input.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("before", s.errors("main.cpp"));
    const before = await s.errors("main.cpp");
    for (const n of [1, 2, 3]) {
        s.edit("main.cpp", { replace: `a() + ${String(n - 1)}`, with: `a() + ${String(n)}` });
        expect(await s.errors("main.cpp")).toEqual(before);
    }
    await s.sync();
    expect((await s.counts()).files["a.cppm"]?.pcm).toBe(1);

    t.disk.edit("a.cppm", { replace: "return broken_in_a;", with: "return 1;" });
    s.save("a.cppm");
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.sync();
    expect((await s.counts()).files["a.cppm"]?.pcm).toBe(2);
});
