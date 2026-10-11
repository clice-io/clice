import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A preamble that imports keeps its PCH, built again once against the
// module rebuilt after a save.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.sync();
    expect((await s.counts()).files["main.cpp"]?.pch).toBe(1);

    t.disk.edit("a.cppm", {
        before: "export int a()",
        insert: "export int b() {\n    return 2;\n}\n\n",
    });
    s.save("a.cppm");
    await s.sync();
    s.edit("main.cpp", { replace: "return a();", with: "return a() + b();" });
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("added", s.hover(t.at("main.cpp", "a() + |b()")));
    await s.sync();
    expect((await s.counts()).files["main.cpp"]?.pch).toBe(2);
});
