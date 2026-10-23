import { test } from "@clice/tools/testing";

// The editor's compile of main.cpp skips the function bodies of shapes.h,
// which it includes at its top: what goes wrong in area()'s body,
// instantiated for Square, shows once the background index has compiled
// main.cpp as saved, while the buffer holds that text.
test("as saved", async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("indexed", s.diagnostics("main.cpp"), { parts: ["related"] });
    s.edit("main.cpp", { replace: "Square{2}", with: "Square{3}" });
    await t.show("edited", s.diagnostics("main.cpp"));
    s.save("main.cpp");
    await s.indexed();
    await t.show("saved", s.diagnostics("main.cpp"));
});

// A header saved with an error in a function body reaches the open file
// including it through that file's background run.
test("a saved header", async (t) => {
    const s = await t.start();
    await s.indexed();
    s.open("shapes.h");
    s.edit("shapes.h", { replace: "return 1;", with: "return missing;" });
    s.save("shapes.h");
    await s.indexed();
    await t.show("saved header", s.diagnostics("main.cpp"));
});

test("bodies parsed", async (t) => {
    const s = await t.start({
        config: { project: { enable_indexing: false, skip_header_bodies: false } },
    });
    await t.show("parsed", s.diagnostics("main.cpp"));
});
