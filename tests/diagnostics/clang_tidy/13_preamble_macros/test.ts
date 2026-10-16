import { test } from "@clice/tools/testing";

// Macros defined among the leading directives, a header guard's too, reach
// the checks looking at macro names as those defined later do.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
    await t.show("guard.h", s.diagnostics("guard.h"));
});
