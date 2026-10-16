import { test } from "@clice/tools/testing";

// The header's unsaved buffer declares the function main.cpp calls: main.cpp
// compiles against the header on disk, as clangd does, until it is saved.
// Serving every open buffer to every compile would rebuild the includers'
// precompiled preambles on each keystroke in the header.
test(async (t) => {
    const s = await t.start();
    s.open("header.h");
    s.edit("header.h", {
        after: "inline int old_func() { return 1; }\n",
        insert: "inline int new_func() { return 2; }\n",
    });
    await t.show("unsaved", s.diagnostics("main.cpp"));
    s.save("header.h");
    await t.show("saved", s.diagnostics("main.cpp"));
});
