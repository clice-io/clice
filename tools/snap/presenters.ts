/// Presenters render wire replies into snapshot bodies. Every file
/// reference goes through normalizeFileUri, so a malformed URI fails the
/// test on every platform instead of only breaking real clients on
/// Windows. Output must stay byte-identical to the snapshot corpus.

import * as proto from "vscode-languageserver-protocol";
import { markerPoints, markerRanges, type AnnotatedSource } from "./annotation.ts";
import type { CliceClient } from "../client/client.ts";
import type { Workspace } from "../client/workspace.ts";
import {
    focusSemanticTokens,
    OffsetConverter,
    semanticTokenFocusOffsets,
    type TokenPiece,
} from "./inspect.ts";
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

/// Decode the LSP delta-encoded token array into rendered pieces.
/// Positions are UTF-16 code units; fixtures are ASCII, where they
/// coincide with string indices.
export function decodeSemanticTokenPieces(
    data: number[],
    lines: string[],
    legend: proto.SemanticTokensLegend,
): TokenPiece[] {
    const out: TokenPiece[] = [];
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
        out.push({ line, character, length: text.length, rendered: entry + " }" });
    }
    return out;
}

export function decodeSemanticTokens(
    data: number[],
    lines: string[],
    legend: proto.SemanticTokensLegend,
): string[] {
    return decodeSemanticTokenPieces(data, lines, legend).map((piece) => piece.rendered);
}

export const presentSemanticTokens: Presenter = async (client, uri, source) => {
    const result = await client.semanticTokensFull(uri);
    const provider = client.initResult?.capabilities.semanticTokensProvider as
        | proto.SemanticTokensOptions
        | undefined;
    if (!provider) {
        throw new Error("server did not advertise a semantic tokens legend");
    }
    const pieces = decodeSemanticTokenPieces(
        result?.data ?? [],
        source.content.split("\n"),
        provider.legend,
    );
    // §-markers focus the snapshot on the marked tokens; a marker no token
    // covers must still render (as `kind: none`), so this cannot early-out
    // on an empty reply.
    const offsets = semanticTokenFocusOffsets(source);
    if (offsets.length > 0) {
        return focusSemanticTokens(pieces, offsets, Buffer.from(source.content));
    }
    return pieces.map((piece) => piece.rendered);
};

function fmtInlayHints(hints: proto.InlayHint[]): string[] {
    const out: string[] = [];
    for (const hint of hints) {
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
}

export const presentInlayHints: Presenter = async (client, uri, source) => {
    // A fixture may scope the request with `§⟦...⟧` range markers — one
    // section per marker; without them the whole document is requested.
    const ranges = markerRanges(source);
    if (ranges.length === 0) {
        const wholeFile: proto.Range = {
            start: { line: 0, character: 0 },
            end: { line: source.content.split("\n").length, character: 0 },
        };
        return fmtInlayHints((await client.inlayHints(uri, wholeFile)) ?? []);
    }
    const map = new OffsetConverter(Buffer.from(source.content));
    const out: string[] = [];
    for (const [name, [begin, end]] of ranges) {
        if (out.length > 0) {
            out.push("");
        }
        out.push(`${name}:`);
        const range: proto.Range = {
            start: map.position(begin),
            end: map.position(end),
        };
        out.push(...fmtInlayHints((await client.inlayHints(uri, range)) ?? []));
    }
    return out;
};

/// The A/B body of a fixture that carries a `config:` overlay: the same
/// markers rendered under default options and under the overlay, so the
/// snapshot pins what the option actually changes. Blank section
/// separators stay unindented — no trailing whitespace in snapshots.
export function abBlocks(defaults: string[], configured: string[]): string[] {
    const indent = (line: string): string => (line === "" ? line : `  ${line}`);
    return ["default:", ...defaults.map(indent), "", "configured:", ...configured.map(indent)];
}

/// A completion item reduced to the fields the snapshot pins, shared by
/// the wire presenter and the standalone renderer. `score` is the fuzzy
/// match score the feature layer stores in sort_text.
export interface CompletionEntry {
    label: string;
    kind: string | null;
    score: number;
    detail: string | null;
    description: string | null;
    edit: string | null;
    newText: string | null;
    snippet: boolean;
    deprecated: boolean;
}

/// Completion and signature replies are open-ended, so a snapshot pins
/// at most ten items — for completion the ten best by score descending,
/// label ascending on ties (the feature layer itself returns candidates
/// unsorted) — and announces the cut so a truncated list never reads as
/// "nothing else matched".
const SNAP_ITEM_LIMIT = 10;

function renderCompletionEntry(entry: CompletionEntry): string {
    let line = `- { label: ${yamlStr(entry.label)}`;
    if (entry.kind !== null) {
        line += `, kind: ${entry.kind}`;
    }
    if (entry.detail !== null) {
        line += `, detail: ${yamlStr(entry.detail)}`;
    }
    if (entry.description !== null) {
        line += `, description: ${yamlStr(entry.description)}`;
    }
    if (entry.edit !== null) {
        line += `, edit: "${entry.edit}"`;
    }
    if (entry.newText !== null && entry.newText !== entry.label) {
        line += `, insert: ${yamlStr(entry.newText)}`;
    }
    if (entry.snippet) {
        line += ", snippet: true";
    }
    if (entry.deprecated) {
        line += ", deprecated: true";
    }
    return line + " }";
}

export function completionLines(entries: CompletionEntry[]): string[] {
    // The feature layer returns candidates unsorted (clang's order is
    // host-dependent) and scores carry architecture-dependent last bits
    // (FMA contraction), so the snapshot order quantizes the score and
    // then falls back to the full rendered line — deterministic on every
    // host, unlike a raw score-then-label sort whose remaining ties kept
    // the host-dependent input order.
    const rendered = entries.map((entry) => ({
        rank: Math.round(entry.score * 1e4),
        line: renderCompletionEntry(entry),
    }));
    rendered.sort((a, b) => b.rank - a.rank || (a.line < b.line ? -1 : a.line > b.line ? 1 : 0));
    const out = rendered.slice(0, SNAP_ITEM_LIMIT).map((entry) => entry.line);
    if (rendered.length > SNAP_ITEM_LIMIT) {
        out.push(`… +${rendered.length - SNAP_ITEM_LIMIT} more`);
    }
    return out;
}

/// A signature reduced to what the snapshot pins: the rendered label with
/// the active parameter bracketed in place (`foo(int x, ⟦int y⟧)`), which
/// pins both the parameter offsets and the active index in one line.
export interface SignatureEntry {
    label: string;
    /// [begin, end) substrings of `label`, one per parameter.
    parameters: [number, number][];
    activeParameter: number | null;
}

export function signatureLines(signatures: SignatureEntry[]): string[] {
    if (signatures.length === 0) {
        return ["NO SIGNATURES"];
    }
    const out: string[] = [];
    for (const signature of signatures.slice(0, SNAP_ITEM_LIMIT)) {
        let label = signature.label;
        const active =
            signature.activeParameter !== null
                ? signature.parameters[signature.activeParameter]
                : undefined;
        if (active !== undefined) {
            const [begin, end] = active;
            label = `${label.slice(0, begin)}⟦${label.slice(begin, end)}⟧${label.slice(end)}`;
        }
        out.push(`- ${label}`);
    }
    if (signatures.length > SNAP_ITEM_LIMIT) {
        out.push(`… +${signatures.length - SNAP_ITEM_LIMIT} more`);
    }
    return out;
}

/// One rendered hover block per marker, shared by the wire presenter and
/// the standalone renderer so `snap: shared` fixtures compare byte-equal:
///
///     name: { range: "1:4-1:9" }
///     <markdown>
///
///     other: NO HOVER
export function hoverBlocks(
    items: [string, { rangeText: string | null; contents: string } | null][],
): string[] {
    const out: string[] = [];
    for (const [name, item] of items) {
        if (out.length > 0) {
            out.push("");
        }
        if (item === null) {
            out.push(`${name}: NO HOVER`);
            continue;
        }
        out.push(`${name}:${item.rangeText === null ? "" : ` { range: "${item.rangeText}" }`}`);
        out.push(...item.contents.split("\n"));
    }
    return out;
}

function wireCompletionEntry(item: proto.CompletionItem): CompletionEntry {
    const edit = item.textEdit;
    if (edit !== undefined && !("range" in edit)) {
        throw new Error("clice always replies with plain TextEdit completion edits");
    }
    return {
        label: item.label,
        kind: item.kind !== undefined ? enumName(proto.CompletionItemKind, item.kind) : null,
        score: item.sortText !== undefined ? Number.parseFloat(item.sortText) : 0,
        detail: item.labelDetails?.detail ?? null,
        description: item.labelDetails?.description ?? null,
        edit: edit !== undefined ? fmtRange(edit.range) : null,
        // Items without a text edit may still carry a bare insertText
        // (import completion appends the closing semicolon through it).
        newText: edit !== undefined ? edit.newText : (item.insertText ?? null),
        snippet: item.insertTextFormat === proto.InsertTextFormat.Snippet,
        deprecated: item.tags?.includes(proto.CompletionItemTag.Deprecated) ?? false,
    };
}

export const presentCodeCompletion: Presenter = async (client, uri, source) => {
    const map = new OffsetConverter(Buffer.from(source.content));
    const out: string[] = [];
    for (const [name, offset] of markerPoints(source)) {
        const pos = map.position(offset);
        const reply = (await client.completionAt(uri, pos.line, pos.character)) ?? [];
        const items = Array.isArray(reply) ? reply : reply.items;
        if (out.length > 0) {
            out.push("");
        }
        out.push(`${name}:`);
        out.push(...completionLines(items.map(wireCompletionEntry)));
    }
    return out;
};

function wireSignatureEntry(signature: proto.SignatureInformation): SignatureEntry {
    const parameters: [number, number][] = [];
    for (const parameter of signature.parameters ?? []) {
        if (typeof parameter.label === "string") {
            throw new Error("clice always replies with [begin, end) parameter labels");
        }
        parameters.push(parameter.label);
    }
    return {
        label: signature.label,
        parameters,
        activeParameter: signature.activeParameter ?? null,
    };
}

export const presentSignatureHelp: Presenter = async (client, uri, source) => {
    const map = new OffsetConverter(Buffer.from(source.content));
    const out: string[] = [];
    for (const [name, offset] of markerPoints(source)) {
        const pos = map.position(offset);
        const reply = await client.signatureHelpAt(uri, pos.line, pos.character);
        if (out.length > 0) {
            out.push("");
        }
        out.push(`${name}:`);
        out.push(...signatureLines((reply?.signatures ?? []).map(wireSignatureEntry)));
    }
    return out;
};

export const presentHover: Presenter = async (client, uri, source) => {
    const items: [string, { rangeText: string | null; contents: string } | null][] = [];
    const map = new OffsetConverter(Buffer.from(source.content));
    for (const [name, offset] of markerPoints(source)) {
        const pos = map.position(offset);
        const hover = await client.hoverAt(uri, pos.line, pos.character);
        if (!hover) {
            items.push([name, null]);
            continue;
        }
        const contents = hover.contents;
        if (typeof contents === "string" || Array.isArray(contents)) {
            throw new Error("clice always replies with MarkupContent hover contents");
        }
        items.push([
            name,
            {
                rangeText: hover.range ? fmtRange(hover.range) : null,
                contents: contents.value,
            },
        ]);
    }
    return hoverBlocks(items);
};
