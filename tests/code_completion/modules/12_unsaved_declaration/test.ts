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

// The unit renamed in its buffer imports neither its new name nor its old.
test("renamed in its own buffer", async (t) => {
    const s = await t.start();
    await s.diagnostics("geometry.cppm");
    s.edit("geometry.cppm", {
        replace: "export module geometry;",
        with: "export module geometry2d;\nimport geo",
    });
    await t.show("own", s.completion(t.at("geometry.cppm", "import geo|")), {
        parts: ["insert", "edit"],
    });
});
