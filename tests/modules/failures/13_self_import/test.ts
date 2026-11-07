import { test } from "@clice/tools/testing";

// A module imports itself: the unit reports it, and so does its importer.
test(async (t) => {
    const s = await t.start();
    await t.show("self.cppm", s.errors("self.cppm"));
    await t.show("main.cpp", s.errors("main.cpp"));
});
