import { test } from "@clice/tools/testing";

// An import names the module an open unit's buffer declares before the file
// is saved.
test("renamed in the buffer", async (t) => {
    const s = await t.start();
    await s.diagnostics("geometry.cppm");
    s.open("main.cpp");
    s.edit("geometry.cppm", {
        replace: "export module geometry;",
        with: "export module geometry2d;",
    });
    await t.show("renamed", s.completion(t.marker("renamed")), { parts: ["insert", "edit"] });
});
