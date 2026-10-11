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
