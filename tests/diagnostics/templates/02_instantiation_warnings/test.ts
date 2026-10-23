import { test } from "@clice/tools/testing";

// A warning from an instantiation shows on the request as an error does,
// saying it comes from the template and pointing at where. The template is
// in a header whose function bodies the editor skips: the background index
// reports it.
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["related"] });
});
