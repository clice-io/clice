import { test } from "@clice/tools/testing";

// A database written on another machine, or before a clean, runs a command
// in a directory that is gone: its relative paths find nothing, and the file
// says why at its top.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
