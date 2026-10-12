import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("unchanged save no recompile", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    s.open("h.h", { pull: false });
    expect(await s.errors("a.cpp")).toEqual([]);
    const call = t.at("a.cpp", "return |helper()");
    await s.hover(call);
    await s.sync();
    const compiled = (await s.counts()).files["a.cpp"]?.compile;
    // Saving the header's unchanged bytes does not make its host stale.
    s.save("h.h");
    await s.hover(call);
    await s.sync();
    expect((await s.counts()).files["a.cpp"]?.compile).toBe(compiled);
    // Control: saving other bytes does.
    s.edit("h.h", { replace: "return 1;", with: "return 2;" });
    s.save("h.h");
    await s.hover(call);
    await s.sync();
    expect((await s.counts()).files["a.cpp"]?.compile).toBe((compiled ?? 0) + 1);
});
