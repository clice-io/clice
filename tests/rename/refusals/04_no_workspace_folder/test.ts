import { test } from "@clice/tools/testing";

// A server started on no workspace folder edits no sources: with no
// project found for the file either, a rename is refused before anything
// is compiled.
test("no project", async (t) => {
    const s = await t.start({ folders: [] });
    s.open("main.cpp");
    const at = t.at("main.cpp", "int |compute();");
    await t.show("prepare", s.prepareRename(at));
    await t.show("rename", s.rename(at, "evaluate"));
});

// A database beside the file makes its directory a project, whose sources
// a rename edits.
test("a project found for the file", async (t) => {
    t.disk.database({ "main.cpp": [] });
    const s = await t.start({ folders: [] });
    s.open("main.cpp");
    await t.show("rename", s.rename(t.at("main.cpp", "int |compute();"), "evaluate"));
});

// A rename edits its project's sources alone: a declaration in a header
// outside the folder stops it, whether the index has reached the header
// or only the open file's compile has.
test("a declaration outside the project", async (t) => {
    t.disk.database({ "app/main.cpp": [] }, "app");
    const s = await t.start({ folders: ["app"] });
    await s.indexed();
    s.open("app/main.cpp");
    await t.show("rename", s.rename(t.at("app/main.cpp", "int |compute(int x)"), "evaluate"));
});

test("a declaration outside the project, not indexed", async (t) => {
    t.disk.database({ "app/main.cpp": [] }, "app");
    const s = await t.start({
        folders: ["app"],
        config: { project: { enable_indexing: false } },
    });
    s.open("app/main.cpp");
    await t.show("rename", s.rename(t.at("app/main.cpp", "int |compute(int x)"), "evaluate"));
});
