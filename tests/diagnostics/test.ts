import { test } from "@clice/tools/testing";

// What main.cpp shows once the background index has compiled it: the
// editor's own compile skips the function bodies of the headers it includes
// at its top, and that run reports what goes wrong inside them.
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["related"] });
});
