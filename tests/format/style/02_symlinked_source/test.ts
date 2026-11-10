import { test } from "@clice/tools/testing";

// The build names src/lib.cpp, a link to third_party/lib.cpp; each
// directory has its own style. The style is the one beside the name the
// build uses, in the editor and on the command line alike.

test("editor", async (t) => {
    t.disk.symlink("src/lib.cpp", "third_party/lib.cpp");
    const s = await t.start();
    s.open("src/lib.cpp");
    await t.show("src/lib.cpp", s.formatting("src/lib.cpp"));
});

test("command", async (t) => {
    t.disk.symlink("src/lib.cpp", "third_party/lib.cpp");
    await t.cli("format");
});
