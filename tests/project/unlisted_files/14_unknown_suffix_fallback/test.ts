import { test } from "@clice/tools/testing";

// No database and no includer: each file compiles as C++ under the builtin
// command, an extensionless header and a `.tcc` body alike.
test(async (t) => {
    const s = await t.start();
    await t.show("inc/Widget", {
        diagnostics: s.diagnostics("inc/Widget"),
        symbols: s.documentSymbols("inc/Widget"),
    });
    await t.show("impl.tcc", {
        diagnostics: s.diagnostics("impl.tcc"),
        symbols: s.documentSymbols("impl.tcc"),
    });
});
