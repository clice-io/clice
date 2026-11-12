import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// main.cpp is open and compiled, then edited: an edit compiles nothing, and
// neither its last compile's index nor its shard describes the buffer any
// more. References asked from other.cpp wait for its compile and list its
// uses all the same.
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    s.open("main.cpp");
    expect(await s.errors("main.cpp")).toEqual([]);
    const area = t.at("other.cpp", "return |area(3)");
    await t.show("clean", s.references(area));
    s.edit("main.cpp", { after: '#include "shapes.h"\n', insert: "// edited\n" });
    await t.show("edited", s.references(area));
    await t.show("asked in main.cpp", s.references(t.at("main.cpp", "return |area(1)")));
    await s.sync();
    await t.show("compiled", s.references(area));
});
