import { test } from "@clice/tools/testing";

// The fix includes two headers into a module unit without a global module
// fragment: one fragment opens before the module declaration and holds both.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cppm", s.diagnostics("main.cppm"));
    await t.show("fixes", s.codeActions(t.marker("finding"), { only: ["quickfix"] }));
});
