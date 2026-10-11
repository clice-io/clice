import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A fails on an import nothing provides; once a unit declares that module,
// A builds again.
test(async (t) => {
    const s = await t.start();
    await t.show("before", s.errors("main.cpp"));
    t.disk.write("m.cppm", "export module M;\nexport int m() {\n    return 1;\n}\n");
    s.save("m.cppm");
    expect(await s.errors("main.cpp")).toEqual([]);
});
