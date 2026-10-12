import { test } from "@clice/tools/testing";

// The fix would include mem.h, which the leading directives already do: the
// include is left out of its edits.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
    await t.show("fixes", s.codeActions(t.marker("finding"), { only: ["quickfix"] }));
});
