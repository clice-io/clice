import { test } from "@clice/tools/testing";

// The include past the leading directives repeats the active one among
// them, which the precompiled preamble holds: the check still sees it. The
// finding underlines what its fix removes, the newline before it first.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
    await t.show("fixes", s.codeActions(t.marker("duplicate"), { only: ["quickfix"] }));
});
