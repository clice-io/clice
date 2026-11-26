import { test } from "@clice/tools/testing";

// main.cpp looks answer.h up as embedded data before it includes it, and
// other.cpp only includes it. Both parses read the header's bytes as the
// compiler does and index its text: the units are indexed once, not over
// and over, and the header's entries are served.
test("indexed once", async (t) => {
    await t.cli("index");
    await t.cli("index");
});

test("header served", async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("answer", s.workspaceSymbols("answer"));
});
