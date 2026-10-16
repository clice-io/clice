import { test } from "@clice/tools/testing";

// The fix removes the only initializer and leaves its ':' to the cleanup
// clang-tidy does after applying fixes: applied, the code still compiles.
test(async (t) => {
    const s = await t.start();
    await t.show("before", s.diagnostics("main.cpp"));
    await t.show("fixes", s.codeActions(t.marker("finding"), { only: ["quickfix"] }));
    await t.show("after", s.applyAction(t.marker("finding"), "remove 'name()'"));
});
