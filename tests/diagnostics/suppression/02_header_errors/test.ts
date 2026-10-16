import { test } from "@clice/tools/testing";

// The first of a header's errors shown on its #include is suppressed: the
// next one shows there instead.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["related"] });
});
