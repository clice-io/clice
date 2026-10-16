import { test } from "@clice/tools/testing";

// The fix includes make.h into a module unit without a global module
// fragment: the include goes into one it opens before the module
// declaration, not above the file's first line.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cppm", s.diagnostics("main.cppm"));
    await t.show("fixes", s.codeActions(t.marker("finding"), { only: ["quickfix"] }));
});
