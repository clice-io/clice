import { test } from "@clice/tools/testing";

// The preamble's include, a comment before its file name, reaches the
// checks: the one after the preamble repeats it.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
