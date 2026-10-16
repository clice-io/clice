import { test } from "@clice/tools/testing";

// A client declaring no hierarchical support takes the symbols flat, each
// at its whole extent beside the one it nests in; one declaring no kinds
// takes only the first eighteen: a struct as a class, an enumerator as an
// enum, the others as a string.
test("flat", async (t) => {
    const s = await t.start({
        capabilities: {
            textDocument: { documentSymbol: { hierarchicalDocumentSymbolSupport: false } },
        },
    });
    s.open("main.cpp");
    await s.sync();
    await t.show("flat", s.documentSymbols("main.cpp"));
});

test("tree in the first kinds", async (t) => {
    const s = await t.start({
        capabilities: {
            textDocument: { documentSymbol: { hierarchicalDocumentSymbolSupport: true } },
        },
    });
    s.open("main.cpp");
    await s.sync();
    await t.show("tree", s.documentSymbols("main.cpp"));
});
