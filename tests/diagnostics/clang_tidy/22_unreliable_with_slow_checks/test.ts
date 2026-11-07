import { test } from "@clice/tools/testing";

// The slow checks run on request; use-after-move, unreliable on code being
// edited, stays out.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
