import type * as proto from "vscode-languageserver-protocol";
import type { AnnotatedSource } from "../annotation.ts";
import { lowerFirst, OffsetConverter, type Feature } from "../render.ts";
import { yamlStr } from "../snapshot.ts";

/// One rendered semantic token piece (multiline tokens arrive pre-split):
/// its position in line/UTF-16-character coordinates plus the finished
/// snapshot line, so marker-focus filtering works on the exact shape both
/// adapters produce.
export interface TokenPiece {
    line: number;
    character: number;
    /// UTF-16 length of the piece text (0 for a blank interior line).
    length: number;
    rendered: string;
}

interface RawSemanticToken {
    range: { begin: number; end: number };
    kind: string;
    modifiers: number;
}

/// Twin of SymbolModifiers::Kind (src/semantic/symbol.h), in bit order.
/// Drift is caught by any shared semantic-tokens snapshot: the server side
/// renders through the live legend, this table renders the inspect side,
/// and the two stop agreeing the moment they differ.
const SYMBOL_MODIFIERS = [
    "Declaration",
    "Definition",
    "Const",
    "Overloaded",
    "Typed",
    "Templated",
    "Deprecated",
    "Deduced",
    "Readonly",
    "Static",
    "Abstract",
    "Virtual",
    "DependentName",
    "DefaultLibrary",
    "UsedAsMutableReference",
    "UsedAsMutablePointer",
    "ConstructorOrDestructor",
    "UserDefined",
];

export function rawSemanticTokenPieces(result: unknown, stripped: Buffer): TokenPiece[] {
    const map = new OffsetConverter(stripped);
    const out: TokenPiece[] = [];
    for (const token of result as RawSemanticToken[]) {
        if (token.range.end <= token.range.begin || token.range.end > stripped.length) {
            continue;
        }
        const kind = lowerFirst(token.kind);
        const modifiers = SYMBOL_MODIFIERS.filter((_name, bit) => token.modifiers & (1 << bit));
        const suffix =
            `, kind: ${kind}` +
            (modifiers.length > 0 ? `, modifiers: [${modifiers.map(lowerFirst).join(", ")}]` : "") +
            " }";

        // LSP semantic tokens are single-line; the server splits multiline
        // tokens into per-line pieces on the reply edge, so mirror that
        // split here to keep shared snapshots comparable.
        const chunk = stripped.subarray(token.range.begin, token.range.end);
        let pieceStart = 0;
        let { line, character } = map.position(token.range.begin);
        for (let i = 0; i <= chunk.length; i++) {
            if (i !== chunk.length && chunk[i] !== 0x0a) {
                continue;
            }
            const text = chunk.subarray(pieceStart, i).toString("utf8");
            // A newline-terminated piece always encodes with length >= 1 (the
            // newline itself), so the server emits it even for a blank
            // interior line and the decoder reconstructs text: "". Only the
            // final, unterminated piece is dropped when empty.
            if (i !== chunk.length || text.length > 0) {
                out.push({
                    line,
                    character,
                    length: text.length,
                    rendered: `- { loc: "${line}:${character}", text: ${yamlStr(text)}${suffix}`,
                });
            }
            line += 1;
            character = 0;
            pieceStart = i + 1;
        }
    }
    return out;
}

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

/// The identifier (or single character) written at `offset`, for labelling
/// a marker that no token covers.
function wordAt(stripped: Buffer, offset: number): string {
    const isWord = (byte: number | undefined): boolean =>
        byte !== undefined &&
        ((byte >= 0x30 && byte <= 0x39) ||
            (byte >= 0x41 && byte <= 0x5a) ||
            (byte >= 0x61 && byte <= 0x7a) ||
            byte === 0x5f);
    let end = offset;
    while (isWord(stripped[end])) {
        end += 1;
    }
    if (end === offset) {
        if (offset >= stripped.length) {
            return "";
        }
        // A non-word position labels with its whole UTF-8 sequence, not a
        // lone byte turned into mojibake.
        let sequenceEnd = offset + 1;
        while (sequenceEnd < stripped.length && ((stripped[sequenceEnd] ?? 0) & 0xc0) === 0x80) {
            sequenceEnd += 1;
        }
        return stripped.subarray(offset, sequenceEnd).toString("utf8");
    }
    let begin = offset;
    while (begin > 0 && isWord(stripped[begin - 1])) {
        begin -= 1;
    }
    return stripped.subarray(begin, end).toString("utf8");
}

/// Marker points that focus a semantic-tokens snapshot, in source order.
/// Ranges have no meaning here — reject them so a fixture doesn't silently
/// pin the full dump its author meant to focus.
export function semanticTokenFocusOffsets(source: AnnotatedSource): number[] {
    if (source.ranges.size > 0) {
        throw new Error("semantic_tokens fixtures take § point markers, not §⟦...⟧ ranges");
    }
    return [...new Set([...source.offsets.values(), ...source.namelessOffsets])].sort(
        (a, b) => a - b,
    );
}

/// Focused snapshot body: one line per marker — the covering token piece,
/// or an explicit `kind: none` entry so a missing token is pinned visibly
/// and its fix flips the snapshot instead of appearing from nothing.
/// Markers are placed on the first character of the token of interest.
export function focusSemanticTokens(
    pieces: TokenPiece[],
    offsets: number[],
    stripped: Buffer,
): string[] {
    const map = new OffsetConverter(stripped);
    return offsets.map((offset) => {
        const pos = map.position(offset);
        const hit = pieces.find(
            (piece) =>
                piece.line === pos.line &&
                piece.character <= pos.character &&
                pos.character < piece.character + piece.length,
        );
        if (hit) {
            return hit.rendered;
        }
        const text = yamlStr(wordAt(stripped, offset));
        return `- { loc: "${pos.line}:${pos.character}", text: ${text}, kind: none }`;
    });
}

/// §-markers focus the snapshot on the marked tokens; a marker no token
/// covers must still render (as `kind: none`), so this cannot early-out on
/// an empty reply.
function formatSemanticTokens(
    pieces: TokenPiece[],
    source: AnnotatedSource,
    stripped: Buffer,
): string[] {
    const offsets = semanticTokenFocusOffsets(source);
    if (offsets.length > 0) {
        return focusSemanticTokens(pieces, offsets, stripped);
    }
    return pieces.map((piece) => piece.rendered);
}

export const semanticTokens: Feature = {
    shape: "document",
    fromInspect(entry, ctx) {
        return formatSemanticTokens(
            rawSemanticTokenPieces(entry.result, ctx.stripped),
            ctx.source,
            ctx.stripped,
        );
    },
    async fromServer(client, uri, ctx) {
        const result = await client.semanticTokensFull(uri);
        const provider = client.initResult?.capabilities.semanticTokensProvider as
            | proto.SemanticTokensOptions
            | undefined;
        if (!provider) {
            throw new Error("server did not advertise a semantic tokens legend");
        }
        const pieces = decodeSemanticTokenPieces(
            result?.data ?? [],
            ctx.source.content.split("\n"),
            provider.legend,
        );
        return formatSemanticTokens(pieces, ctx.source, ctx.stripped);
    },
};
