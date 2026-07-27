/// Presenters render wire replies into snapshot bodies. Every file
/// reference goes through normalizeFileUri, so a malformed URI fails the
/// test on every platform instead of only breaking real clients on
/// Windows. Output must stay byte-identical to the snapshot corpus.

import * as proto from "vscode-languageserver-protocol";
import type { AnnotatedSource } from "./annotation.ts";
import type { CliceClient } from "../client/client.ts";
import type { Workspace } from "../client/workspace.ts";
import { normalizeFileUri, yamlStr } from "./snapshot.ts";

export type Presenter = (
    client: CliceClient,
    uri: string,
    source: AnnotatedSource,
    workspace: Workspace,
) => Promise<string[]>;

export function fmtPos(pos: proto.Position): string {
    return `${pos.line}:${pos.character}`;
}

export function fmtRange(range: proto.Range): string {
    return `${fmtPos(range.start)}-${fmtPos(range.end)}`;
}

function enumName(enumObject: Record<string, unknown>, value: number): string {
    for (const [name, v] of Object.entries(enumObject)) {
        if (v === value) {
            return name;
        }
    }
    return String(value);
}

export const presentDocumentLinks: Presenter = async (client, uri, _source, workspace) => {
    const links = (await client.documentLinks(uri)) ?? [];
    return links.map((link) => {
        if (link.target === undefined) {
            throw new Error("clice always resolves link targets");
        }
        return (
            `- { range: "${fmtRange(link.range)}", ` +
            `target: ${yamlStr(normalizeFileUri(link.target, workspace.root))} }`
        );
    });
};

export const presentFoldingRanges: Presenter = async (client, uri) => {
    const out: string[] = [];
    for (const r of (await client.foldingRanges(uri)) ?? []) {
        let line = `- { range: "${r.startLine}:${r.startCharacter}-${r.endLine}:${r.endCharacter}"`;
        if (r.kind !== undefined) {
            line += `, kind: ${r.kind}`;
        }
        if (r.collapsedText) {
            line += `, collapsed_text: ${yamlStr(r.collapsedText)}`;
        }
        out.push(line + " }");
    }
    return out;
};

export const presentDocumentSymbols: Presenter = async (client, uri) => {
    const symbols = (await client.documentSymbols(uri)) ?? [];
    const out: string[] = [];

    const walk = (symbol: proto.DocumentSymbol, depth: number): void => {
        let line =
            `-${" ".repeat(1 + 2 * depth)}{ name: ${yamlStr(symbol.name)}, ` +
            `kind: ${enumName(proto.SymbolKind, symbol.kind)}, ` +
            `range: "${fmtRange(symbol.range)}", ` +
            `selection_range: "${fmtRange(symbol.selectionRange)}"`;
        if (symbol.detail) {
            line += `, detail: ${yamlStr(symbol.detail)}`;
        }
        out.push(line + " }");
        for (const child of symbol.children ?? []) {
            walk(child, depth + 1);
        }
    };

    for (const symbol of symbols) {
        if (!("range" in symbol)) {
            throw new Error("expected hierarchical document symbols");
        }
        walk(symbol, 0);
    }
    return out;
};

/// Decode the LSP delta-encoded token array into presenter lines.
/// Positions are UTF-16 code units; fixtures are ASCII, where they
/// coincide with string indices.
export function decodeSemanticTokens(
    data: number[],
    lines: string[],
    legend: proto.SemanticTokensLegend,
): string[] {
    const out: string[] = [];
    let line = 0;
    let character = 0;
    for (let i = 0; i + 4 < data.length; i += 5) {
        // The loop bound proves these five reads are in range.
        const [deltaLine, deltaStart, length, kind, modifiers] = [
            data[i] ?? 0,
            data[i + 1] ?? 0,
            data[i + 2] ?? 0,
            data[i + 3] ?? 0,
            data[i + 4] ?? 0,
        ];
        line += deltaLine;
        character = deltaLine === 0 ? character + deltaStart : deltaStart;
        const text = (lines[line] ?? "").slice(character, character + length);
        let entry = `- { loc: "${line}:${character}", text: ${yamlStr(text)}, kind: ${legend.tokenTypes[kind]}`;
        const names = legend.tokenModifiers.filter((_name, bit) => modifiers & (1 << bit));
        if (names.length > 0) {
            entry += `, modifiers: [${names.join(", ")}]`;
        }
        out.push(entry + " }");
    }
    return out;
}

export const presentSemanticTokens: Presenter = async (client, uri, source) => {
    const result = await client.semanticTokensFull(uri);
    if (!result || result.data.length === 0) {
        return [];
    }
    const provider = client.initResult?.capabilities.semanticTokensProvider as
        | proto.SemanticTokensOptions
        | undefined;
    if (!provider) {
        throw new Error("server did not advertise a semantic tokens legend");
    }
    return decodeSemanticTokens(result.data, source.content.split("\n"), provider.legend);
};

export const presentInlayHints: Presenter = async (client, uri, source) => {
    const wholeFile: proto.Range = {
        start: { line: 0, character: 0 },
        end: { line: source.content.split("\n").length, character: 0 },
    };
    const out: string[] = [];
    for (const hint of (await client.inlayHints(uri, wholeFile)) ?? []) {
        const label =
            typeof hint.label === "string"
                ? hint.label
                : hint.label.map((part) => part.value).join("");
        let line = `- { pos: "${fmtPos(hint.position)}"`;
        if (hint.kind !== undefined) {
            line += `, kind: ${enumName(proto.InlayHintKind, hint.kind)}`;
        }
        line += `, label: ${yamlStr(label)}`;
        if (hint.paddingLeft) {
            line += ", padding_left: true";
        }
        if (hint.paddingRight) {
            line += ", padding_right: true";
        }
        out.push(line + " }");
    }
    return out;
};
