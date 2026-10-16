import { test } from "@clice/tools/testing";

// One instantiation raises a warning and an error: both show on the
// request, the warning not hiding the error.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["related"] });
});
