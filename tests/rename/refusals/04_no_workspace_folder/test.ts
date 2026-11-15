import { test } from "@clice/tools/testing";

// A file outside every workspace folder, with no project found for it
// either: a rename whose edits all lie in the file is made.
test("no project", async (t) => {
    const s = await t.start({ folders: [] });
    s.open("main.cpp");
    const at = t.at("main.cpp", "int |compute();");
    await t.show("prepare", s.prepareRename(at));
    await t.show("rename", s.rename(at, "evaluate"));
});

// One that would change the header beside the file is refused: the header
// lies in no folder either.
test("a declaration beside the file", async (t) => {
    const s = await t.start({ folders: [] });
    s.open("lone/main.cpp");
    const at = t.at("lone/main.cpp", "return |scale(2)");
    await t.show("prepare", s.prepareRename(at));
    await t.show("rename", s.rename(at, "resize"));
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
        lastRule: { background_index: false },
    });
    s.open("app/main.cpp");
    await t.show("rename", s.rename(t.at("app/main.cpp", "int |compute(int x)"), "evaluate"));
});
