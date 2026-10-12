import { test } from "@clice/tools/testing";

// Every request the server advertises is answered on a plain file: asked
// right after the open, before any compile finished, and once it compiled.

test("before the compile", async (t) => {
    const s = await t.start();
    s.open("main.cpp", { pull: false });
    await t.show("hover", s.hover(t.marker("add")));
    await t.show("diagnostics", s.diagnostics("main.cpp"));
});

test("after the compile", async (t) => {
    const s = await t.start();
    await t.show("diagnostics", s.diagnostics("main.cpp"));
    await t.show("hover", s.hover(t.marker("add")));
    await t.show("completion", s.completion(t.marker("member")));
    await t.show("signature help", s.signatureHelp(t.marker("argument")));
    await t.show("definition", s.definition(t.marker("call")));
    await t.show("highlights", s.highlights(t.marker("result")));
    await t.show("symbols", s.documentSymbols("main.cpp"));
    await t.show("folds", s.foldingRanges("main.cpp"));
    await t.show("tokens", s.semanticTokens("main.cpp"));
    await t.show("links", s.documentLinks("main.cpp"));
    await t.show("hints", s.inlayHints("main.cpp"));
    await t.show("actions", s.codeActions(t.marker("add")));
    await t.show("selection", s.selectionRange(t.marker("argument")));
});
