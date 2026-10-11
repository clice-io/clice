import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// An include added in the buffer, of a header that imports, builds the
// module before the unit compiles.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    s.edit("main.cpp", { text: '#include "deps.h"\nint main() {\n    return a();\n}\n' });
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("use", s.hover(t.at("main.cpp", "return |a()")));
});
