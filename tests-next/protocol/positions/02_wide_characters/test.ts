import { test } from "@clice/tools/testing";

// Positions count UTF-16 code units, as LSP asks of a client that offers no
// other encoding: characters outside ASCII before a name on its line, one
// of them outside the Basic Multilingual Plane, move nothing.

test(async (t) => {
    const s = await t.start();
    await t.show("diagnostics", s.diagnostics("main.cpp"));
    await t.show("hover", s.hover(t.marker("sum")));
    await t.show("definition", s.definition(t.marker("call")));
    await t.show("highlights", s.highlights(t.marker("sum")));
    await t.show("tokens", s.semanticTokens("main.cpp"));
    await t.show("hints", s.inlayHints("main.cpp"));
});
