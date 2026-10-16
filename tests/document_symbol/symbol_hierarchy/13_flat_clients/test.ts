import { expect } from "vitest";
import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// A client declaring no hierarchical support takes the symbols flat, each
// naming the one it nests in; one declaring no kinds takes only the first
// eighteen, a struct as a class and an enumerator as an enum.
test("flat", async (t) => {
    const s = await t.start({
        capabilities: {
            textDocument: { documentSymbol: { hierarchicalDocumentSymbolSupport: false } },
        },
    });
    s.open("main.cpp");
    await s.sync();
    const reply = await s.request<proto.SymbolInformation[]>(
        "textDocument/documentSymbol",
        "main.cpp",
    );
    expect(reply.map(({ name, kind, containerName }) => ({ name, kind, containerName }))).toEqual([
        { name: "shapes", kind: proto.SymbolKind.Namespace, containerName: undefined },
        { name: "Circle", kind: proto.SymbolKind.Class, containerName: "shapes" },
        { name: "radius", kind: proto.SymbolKind.Field, containerName: "Circle" },
        { name: "area", kind: proto.SymbolKind.Method, containerName: "Circle" },
        { name: "Kind", kind: proto.SymbolKind.Enum, containerName: "shapes" },
        { name: "Round", kind: proto.SymbolKind.Enum, containerName: "Kind" },
    ]);
    expect(reply.every((symbol) => symbol.location.uri === s.uri("main.cpp"))).toBe(true);
});

test("tree in the first kinds", async (t) => {
    const s = await t.start({
        capabilities: {
            textDocument: { documentSymbol: { hierarchicalDocumentSymbolSupport: true } },
        },
    });
    s.open("main.cpp");
    await s.sync();
    const reply = await s.request<proto.DocumentSymbol[]>(
        "textDocument/documentSymbol",
        "main.cpp",
    );
    const kinds = (symbols: proto.DocumentSymbol[]): unknown[] =>
        symbols.map(({ name, kind, children }) => ({
            name,
            kind,
            children: kinds(children ?? []),
        }));
    expect(kinds(reply)).toEqual([
        {
            name: "shapes",
            kind: proto.SymbolKind.Namespace,
            children: [
                {
                    name: "Circle",
                    kind: proto.SymbolKind.Class,
                    children: [
                        { name: "radius", kind: proto.SymbolKind.Field, children: [] },
                        { name: "area", kind: proto.SymbolKind.Method, children: [] },
                    ],
                },
                {
                    name: "Kind",
                    kind: proto.SymbolKind.Enum,
                    children: [{ name: "Round", kind: proto.SymbolKind.Enum, children: [] }],
                },
            ],
        },
    ]);
});
