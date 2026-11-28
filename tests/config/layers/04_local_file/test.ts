import { test } from "@clice/tools/testing";

// The local file's rule decides first and edits last: its -std=c++20 stands
// after the project's -std=c++17, and its remove takes the project's macro.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
