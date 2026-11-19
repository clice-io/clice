import { test } from "@clice/tools/testing";

// The editor serves the open file from an in-memory buffer. A check must see
// the buffer as the file, not as clang's unnamed synthetic input.
test("editor", async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});

test("lint", async (t) => {
    await t.cli("lint --jobs 1");
});
