import { test } from "@clice/tools/testing";

// A preamble macro whose name a line splice runs through reaches the checks
// under the name it spells.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
