import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A's buffer imports B, unsaved, while B is saved.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    s.open("a.cppm");
    s.edit("a.cppm", { after: "export module A;\n", insert: "import B;\n" });
    expect(await s.errors("a.cppm")).toEqual([]);
    t.disk.edit("b.cppm", { replace: "return 2;", with: "return 3;" });
    s.save("b.cppm");
    s.edit("main.cpp", { replace: "return a();", with: "return a() + 0;" });
    expect(await s.errors("main.cpp")).toEqual([]);
});
