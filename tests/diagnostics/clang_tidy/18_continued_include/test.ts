import { test } from "@clice/tools/testing";

// The preamble's include, continued onto the next line, reaches the
// checks: the one after the preamble repeats it.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
