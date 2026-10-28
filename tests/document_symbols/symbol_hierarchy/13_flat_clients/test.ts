import { expect } from "vitest";
import type * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

const FLAT = { textDocument: { documentSymbol: { hierarchicalDocumentSymbolSupport: false } } };

// A client declaring no hierarchical support takes the symbols flat, each
// at its whole extent beside the one it nests in; one declaring no kinds
// takes only the first eighteen: a struct as a class, an enumerator as an
// enum, the others as a string.
test("flat", async (t) => {
    const s = await t.start({ capabilities: FLAT });
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

// A second spelling of an open file is served by the first one's document;
// its flat symbols still sit in the document the request names, which the
// rendered paths, resolving links, cannot tell apart.
test(
    "flat at the spelling asked",
    { except: { win32: "a symlink on Windows needs a privilege the CI runners lack" } },
    async (t) => {
        t.disk.symlink("alias.cpp", "main.cpp");
        const s = await t.start({ capabilities: FLAT });
        s.open("main.cpp");
        s.open("alias.cpp");
        await s.sync();
        const reply = await s.request<proto.SymbolInformation[]>(
            "textDocument/documentSymbol",
            "alias.cpp",
        );
        expect(new Set(reply.map((symbol) => s.spelling(symbol.location.uri)))).toEqual(
            new Set(["alias.cpp"]),
        );
    },
);
