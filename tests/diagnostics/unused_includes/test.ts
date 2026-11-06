import { test } from "@clice/tools/testing";

// The warnings come from the background index's compile of the open file,
// which parses the function bodies the editor's own compile skips.
test(async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    await s.indexed();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["tags"] });
});
