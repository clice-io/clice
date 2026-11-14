import { test } from "@clice/tools/testing";

// Turned off, the slow checks are left out without naming them.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
    await t.show("guidance", await s.guidance());
});
