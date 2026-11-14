import { test } from "@clice/tools/testing";

// The slow checks run on request; use-after-move, unreliable on code being
// edited, stays out, while `clice lint` finds what it finds here.
test("editor", async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});

test("lint", async (t) => {
    await t.cli("lint --jobs 1");
});
