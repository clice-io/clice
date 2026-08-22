import * as proto from "vscode-languageserver-protocol";
import { enumName, fmtRange, OffsetConverter, type Feature } from "../render.ts";
import { yamlStr } from "../snapshot.ts";

interface SymbolEntry {
    name: string;
    /// LSP SymbolKind name, the vocabulary snapshots pin.
    kind: string;
    range: string;
    selectionRange: string;
    detail: string;
    children: SymbolEntry[];
}

interface RawDocumentSymbol {
    name: string;
    detail: string;
    kind: string;
    range: { begin: number; end: number };
    selection_range: { begin: number; end: number };
    children: RawDocumentSymbol[];
}

/// Twin of to_protocol_symbol_kind (src/feature/document_symbols.cpp):
/// clice SymbolKind names to the LSP SymbolKind names the server replies
/// with. Anything unlisted maps to Variable, like the C++ default.
const LSP_SYMBOL_KIND: Record<string, string> = {
    Module: "Module",
    Namespace: "Namespace",
    Class: "Class",
    Struct: "Struct",
    Union: "Class",
    Enum: "Enum",
    Type: "Class",
    Concept: "TypeParameter",
    Field: "Field",
    EnumMember: "EnumMember",
    Function: "Function",
    Method: "Method",
    Macro: "Constant",
    Comment: "String",
    Character: "String",
    String: "String",
    Header: "String",
    Number: "Number",
    Operator: "Operator",
    Paren: "Operator",
    Bracket: "Operator",
    Brace: "Operator",
    Angle: "Operator",
};

function formatDocumentSymbols(symbols: SymbolEntry[]): string[] {
    const out: string[] = [];
    const walk = (symbol: SymbolEntry, depth: number): void => {
        let line =
            `-${" ".repeat(1 + 2 * depth)}{ name: ${yamlStr(symbol.name)}, ` +
            `kind: ${symbol.kind}, ` +
            `range: "${symbol.range}", ` +
            `selection_range: "${symbol.selectionRange}"`;
        if (symbol.detail) {
            line += `, detail: ${yamlStr(symbol.detail)}`;
        }
        out.push(line + " }");
        for (const child of symbol.children) {
            walk(child, depth + 1);
        }
    };
    for (const symbol of symbols) {
        walk(symbol, 0);
    }
    return out;
}

export const documentSymbol: Feature = {
    shape: "document",
    fromInspect(entry, ctx) {
        const map = new OffsetConverter(ctx.stripped);
        const fmt = (range: { begin: number; end: number }): string => {
            const start = map.position(range.begin);
            const end = map.position(range.end);
            return `${start.line}:${start.character}-${end.line}:${end.character}`;
        };
        const adapt = (symbol: RawDocumentSymbol): SymbolEntry => ({
            name: symbol.name,
            kind: LSP_SYMBOL_KIND[symbol.kind] ?? "Variable",
            range: fmt(symbol.range),
            selectionRange: fmt(symbol.selection_range),
            detail: symbol.detail,
            children: symbol.children.map(adapt),
        });
        return formatDocumentSymbols((entry.result as RawDocumentSymbol[]).map(adapt));
    },
    async fromServer(client, uri) {
        const symbols = (await client.documentSymbols(uri)) ?? [];
        const adapt = (symbol: proto.DocumentSymbol): SymbolEntry => ({
            name: symbol.name,
            kind: enumName(proto.SymbolKind, symbol.kind),
            range: fmtRange(symbol.range),
            selectionRange: fmtRange(symbol.selectionRange),
            detail: symbol.detail ?? "",
            children: (symbol.children ?? []).map(adapt),
        });
        return formatDocumentSymbols(
            symbols.map((symbol) => {
                if (!("range" in symbol)) {
                    throw new Error("expected hierarchical document symbols");
                }
                return adapt(symbol);
            }),
        );
    },
};
