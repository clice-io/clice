import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const FIRST =
    'module;\n#include "util.h"\nexport module first;\nexport int f() { return util(); }\n';

test("first module borrows project flags", async (t) => {
    const s = await t.start();
    t.disk.write("src/first.cppm", FIRST);
    // The module compiles under the .cpp unit's flags.
    expect(await s.errors("src/first.cppm")).toEqual([]);
    s.save("src/first.cppm");
    // The database unit imports it.
    s.open("src/a.cpp");
    s.edit("src/a.cpp", { text: "import first;\nint a() { return f(); }\n" });
    expect(await s.errors("src/a.cpp")).toEqual([]);
});
