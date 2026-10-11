import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A preamble import nothing provides gets its module once a unit declares
// it.
test(async (t) => {
    const s = await t.start();
    await t.show("before", s.errors("main.cpp"));
    t.disk.write("a.cppm", "export module A;\nexport int a() {\n    return 1;\n}\n");
    s.save("a.cppm");
    expect(await s.errors("main.cpp")).toEqual([]);
});
