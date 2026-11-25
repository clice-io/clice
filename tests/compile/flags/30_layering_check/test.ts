import { test } from "@clice/tools/testing";

// clice builds no header module and leaves the layering check out: both
// includes compile clean.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
