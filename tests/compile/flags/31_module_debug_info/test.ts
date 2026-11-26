import { test } from "@clice/tools/testing";

// clice builds its precompiled files in its own container: the file
// compiles clean.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
