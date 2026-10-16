import { test } from "@clice/tools/testing";

// A warning from an instantiation shows on the request as an error does,
// saying it comes from the template and pointing at where.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["related"] });
});
