import { test } from "@clice/tools/testing";

// The root's clice.toml does not parse, which stops the root's project
// alone: sub/, a project of its own with its clice.toml and database, still
// serves its files and reports the undeclared name.
test("a broken outer project", async (t) => {
    const s = await t.start();
    await t.show("sub/main.cpp", s.diagnostics("sub/main.cpp"));
});

// sub/'s clice.toml does not parse: its files stay with it, answering
// nothing, rather than going to the root's project, whose database lists
// them too.
test("a broken inner project", async (t) => {
    t.disk.write("clice.toml", "[project]\nenable_indexing = false\n");
    t.disk.write("sub/clice.toml", "[project\n");
    t.disk.database({ "sub/main.cpp": [] });
    const s = await t.start({ folders: [".", "sub"] });
    await t.show("sub/main.cpp", s.diagnostics("sub/main.cpp"));
});
