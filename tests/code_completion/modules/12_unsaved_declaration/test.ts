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

// A partition its buffer exports is an interface partition, which the
// interface imports, before the file is saved.
test("exported in the buffer", async (t) => {
    const s = await t.start();
    await s.diagnostics("shapes_detail.cppm");
    s.open("shapes.cppm");
    s.edit("shapes_detail.cppm", {
        replace: "module shapes:detail;",
        with: "export module shapes:detail;",
    });
    s.edit("shapes.cppm", {
        replace: "export module shapes;",
        with: "export module shapes;\nimport :",
    });
    await t.show("exported", s.completion(t.at("shapes.cppm", "import :|")), {
        parts: ["insert", "edit"],
    });
});

// A file the graph knows as no module declares one in its buffer, which an
// import names before the file is saved.
test("declared in the buffer", async (t) => {
    const s = await t.start();
    await s.diagnostics("sketch.cpp");
    s.open("main.cpp");
    s.edit("sketch.cpp", { replace: "int sketch = 0;", with: "export module sketchpad;" });
    s.edit("main.cpp", { replace: "import geo", with: "import sketch" });
    await t.show("declared", s.completion(t.at("main.cpp", "import sketch|")), {
        parts: ["insert", "edit"],
    });
});
