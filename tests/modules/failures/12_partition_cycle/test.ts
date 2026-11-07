import { test } from "@clice/tools/testing";

// Two partitions of one module import each other: neither builds, nor
// does the module's interface, and an importer of the module reports why.
test(async (t) => {
    const s = await t.start();
    await t.show("p1.cppm", s.errors("p1.cppm"));
    await t.show("p2.cppm", s.errors("p2.cppm"));
    await t.show("m.cppm", s.errors("m.cppm"));
    await t.show("main.cpp", s.errors("main.cpp"));
});
