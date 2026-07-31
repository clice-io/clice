/// Driver for `clice inspect`: run a feature over a fixture corpus without
/// a server and render the raw payloads into the same snapshot lines the
/// wire presenters produce. Raw results carry byte offsets into the
/// annotation-stripped source; every conversion the server performs on the
/// reply edge (position mapping, delta encoding, vocabulary) is re-derived
/// here independently, so a `snap: shared` fixture pins both paths against
/// one file and a divergence fails the suite instead of hiding.

import { execFile, execFileSync } from "node:child_process";
import * as crypto from "node:crypto";
import { promisify } from "node:util";
import { normalizeFilePath, yamlStr } from "./snapshot.ts";

export interface InspectFileEntry {
    stripped_hash: string;
    result?: unknown;
    /// Marker-driven payloads keyed by annotation name; null records a
    /// marker with no result (e.g. hover on whitespace).
    markers?: Record<string, unknown> | null;
    error?: string | null;
    diagnostics?: string[] | null;
}

export interface InspectOutput {
    feature: string;
    files: Record<string, InspectFileEntry>;
}

/// Both helpers pass --annotations: they exist to drive fixture corpora,
/// where inline §-markers are part of the grammar. Plain `clice inspect`
/// (no flag) compiles sources verbatim.
export function runInspect(executable: string, feature: string, path: string): InspectOutput {
    const stdout = execFileSync(executable, ["inspect", "--annotations", feature, path], {
        encoding: "utf8",
        maxBuffer: 64 * 1024 * 1024,
        timeout: 300_000,
    });
    return JSON.parse(stdout) as InspectOutput;
}

/// Async variant for callers that fan inspect processes out in parallel.
export async function runInspectAsync(
    executable: string,
    feature: string,
    path: string,
): Promise<InspectOutput> {
    const { stdout } = await promisify(execFile)(
        executable,
        ["inspect", "--annotations", feature, path],
        {
            encoding: "utf8",
            maxBuffer: 64 * 1024 * 1024,
            timeout: 300_000,
        },
    );
    return JSON.parse(stdout) as InspectOutput;
}

export function sha256(data: Buffer): string {
    return crypto.createHash("sha256").update(data).digest("hex");
}

/// Fixture doc-header metadata that controls the snap suites. Parsing is
/// strict: an unknown key is an error, not a silently ignored typo — a
/// misspelled `snap:` would otherwise disable the shared-snapshot
/// assertion for that fixture without anyone noticing.
export interface FixtureMeta {
    status: "supported" | "partial" | "unsupported";
    /// shared: standalone and wire results must render identically and are
    /// pinned by one snapshot file. separate: the difference is a known
    /// property of the feature; each path pins its own file. skip: the two
    /// paths disagree in a way that is simply wrong (not yet supported) —
    /// both suites skip the fixture and no snapshot exists until fixed.
    snap: "shared" | "separate" | "skip";
}

const META_KEYS = ["status", "issues", "order", "snap"];

export function parseFixtureMeta(content: string, filePath: string): FixtureMeta {
    const meta: FixtureMeta = { status: "supported", snap: "shared" };

    let first = (content.split("\n", 1)[0] ?? "").trim();
    if (!first.startsWith("///")) {
        return meta;
    }
    first = first.slice(3);
    if (first.startsWith(" ")) {
        first = first.slice(1);
    }
    if (!first.startsWith("# ")) {
        return meta;
    }

    // Scan only the metadata list: heading lines and at most one blank
    // separator before it, then `- key: value` lines until the next blank
    // `///`. The markdown description after that may legitimately contain
    // bulleted `word:` lines (feature_docs renders it) — mirroring
    // parseFixture in tools/feature_docs.ts, they are not metadata.
    let inMeta = false;
    const seen = new Set<string>();
    for (let line of content.split("\n")) {
        line = line.trim();
        if (!line.startsWith("///")) {
            break;
        }
        line = line.slice(3).trim();
        if (line.startsWith("#")) {
            continue;
        }
        if (line === "") {
            if (inMeta) {
                break;
            }
            continue;
        }
        // Any `- something:` at the metadata position is treated as a key so
        // that a misspelling (`- Snap:`, `- snap :`) errors instead of
        // silently ending the block on defaults.
        const match = /^- ([^:]+):(.*)$/.exec(line);
        if (!match) {
            break;
        }
        inMeta = true;
        const key = (match[1] ?? "").trim();
        const value = (match[2] ?? "").trim();
        if (!META_KEYS.includes(key)) {
            throw new Error(`${filePath}: unknown fixture meta key '${key}'`);
        }
        // A repeated key (merge leftovers, copy/paste) must not silently
        // let the later value win — it could flip a snap mode unnoticed.
        if (seen.has(key)) {
            throw new Error(`${filePath}: duplicate fixture meta key '${key}'`);
        }
        seen.add(key);
        if (key === "status") {
            if (value !== "supported" && value !== "partial" && value !== "unsupported") {
                throw new Error(`${filePath}: invalid status '${value}'`);
            }
            meta.status = value;
        } else if (key === "snap") {
            if (value !== "shared" && value !== "separate" && value !== "skip") {
                throw new Error(`${filePath}: invalid snap mode '${value}'`);
            }
            meta.snap = value;
        }
    }
    return meta;
}

/// Byte offset -> LSP position over the stripped source. Lines are byte
/// ranges split at '\n'; characters count UTF-16 code units within the
/// line, matching the server's default position encoding.
export class OffsetConverter {
    private lineStarts: number[] = [0];
    private data: Buffer;

    // Plain field assignment: parameter properties are not erasable syntax,
    // and tools/ scripts run under bare `node` in strip-only TS mode.
    constructor(data: Buffer) {
        this.data = data;
        for (let i = 0; i < data.length; i++) {
            if (data[i] === 0x0a) {
                this.lineStarts.push(i + 1);
            }
        }
    }

    position(offset: number): { line: number; character: number } {
        let lo = 0;
        let hi = this.lineStarts.length - 1;
        while (lo < hi) {
            const mid = (lo + hi + 1) >> 1;
            if ((this.lineStarts[mid] ?? 0) <= offset) {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }
        const start = this.lineStarts[lo] ?? 0;
        return { line: lo, character: this.data.subarray(start, offset).toString("utf8").length };
    }
}

interface RawRange {
    begin: number;
    end: number;
}

interface RawFoldingRange {
    range: RawRange;
    kind?: string | null;
    collapsed_text: string;
}

interface RawSemanticToken {
    range: RawRange;
    kind: string;
    modifiers: number;
}

/// C++ enum names arrive PascalCase; the server's semantic-tokens legend
/// lowercases the first letter, and snapshots use that wire spelling.
export function lowerFirst(name: string): string {
    return name.slice(0, 1).toLowerCase() + name.slice(1);
}

/// Twin of SymbolModifiers::Kind (src/semantic/symbol.h), in bit
/// order. Drift is caught by any shared semantic-tokens snapshot: the wire
/// side renders through the server's live legend, this table renders the
/// standalone side, and the two stop agreeing the moment they differ.
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

export type RawRenderer = (result: unknown, stripped: Buffer) => string[];

export const renderRawFoldingRanges: RawRenderer = (result, stripped) => {
    const map = new OffsetConverter(stripped);
    const out: string[] = [];
    for (const r of result as RawFoldingRange[]) {
        const start = map.position(r.range.begin);
        const end = map.position(r.range.end);
        let line = `- { range: "${start.line}:${start.character}-${end.line}:${end.character}"`;
        if (r.kind != null) {
            line += `, kind: ${r.kind}`;
        }
        if (r.collapsed_text) {
            line += `, collapsed_text: ${yamlStr(r.collapsed_text)}`;
        }
        out.push(line + " }");
    }
    return out;
};

/// One rendered semantic token piece (multiline tokens arrive pre-split):
/// its position in line/UTF-16-character coordinates plus the finished
/// snapshot line, so marker-focus filtering works on the exact shape both
/// the standalone and the wire renderer produce.
export interface TokenPiece {
    line: number;
    character: number;
    /// UTF-16 length of the piece text (0 for a blank interior line).
    length: number;
    rendered: string;
}

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
            // interior line and the wire decoder reconstructs text: "". Only
            // the final, unterminated piece is dropped when empty.
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

export const renderRawSemanticTokens: RawRenderer = (result, stripped) =>
    rawSemanticTokenPieces(result, stripped).map((piece) => piece.rendered);

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
export function semanticTokenFocusOffsets(source: {
    offsets: Map<string, number>;
    ranges: Map<string, [number, number]>;
    namelessOffsets: number[];
}): number[] {
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

interface RawDocumentSymbol {
    name: string;
    detail: string;
    kind: string;
    range: RawRange;
    selection_range: RawRange;
    children: RawDocumentSymbol[];
}

/// Twin of to_protocol_symbol_kind (src/feature/document_symbols.cpp):
/// clice SymbolKind names to the LSP SymbolKind names the wire presenter
/// prints. Anything unlisted maps to Variable, like the C++ default.
const LSP_SYMBOL_KIND: Record<string, string> = {
    Module: "Module",
    Namespace: "Namespace",
    Class: "Class",
    Struct: "Struct",
    Union: "Class",
    Enum: "Enum",
    Type: "TypeParameter",
    Concept: "TypeParameter",
    Field: "Field",
    EnumMember: "EnumMember",
    Function: "Function",
    Method: "Method",
    Macro: "Function",
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

export const renderRawDocumentSymbols = (result: unknown, stripped: Buffer): string[] => {
    const map = new OffsetConverter(stripped);
    const fmt = (range: RawRange): string => {
        const start = map.position(range.begin);
        const end = map.position(range.end);
        return `${start.line}:${start.character}-${end.line}:${end.character}`;
    };
    const out: string[] = [];
    const walk = (symbol: RawDocumentSymbol, depth: number): void => {
        let line =
            `-${" ".repeat(1 + 2 * depth)}{ name: ${yamlStr(symbol.name)}, ` +
            `kind: ${LSP_SYMBOL_KIND[symbol.kind] ?? "Variable"}, ` +
            `range: "${fmt(symbol.range)}", ` +
            `selection_range: "${fmt(symbol.selection_range)}"`;
        if (symbol.detail) {
            line += `, detail: ${yamlStr(symbol.detail)}`;
        }
        out.push(line + " }");
        for (const child of symbol.children) {
            walk(child, depth + 1);
        }
    };
    for (const symbol of result as RawDocumentSymbol[]) {
        walk(symbol, 0);
    }
    return out;
};

interface RawInlayHint {
    offset: number;
    kind: string;
    label: string;
    padding_left: boolean;
    padding_right: boolean;
}

/// Twin of the HintCategory -> protocol::InlayHintKind switch in
/// src/feature/inlay_hints.cpp.
const LSP_INLAY_KIND: Record<string, string> = {
    Parameter: "Parameter",
    DefaultArgument: "Parameter",
    Type: "Type",
    Designator: "Type",
    BlockEnd: "Type",
};

export const renderRawInlayHints = (result: unknown, stripped: Buffer): string[] => {
    const map = new OffsetConverter(stripped);
    const out: string[] = [];
    for (const hint of result as RawInlayHint[]) {
        const pos = map.position(hint.offset);
        let line = `- { pos: "${pos.line}:${pos.character}"`;
        const kind = LSP_INLAY_KIND[hint.kind];
        if (kind === undefined) {
            throw new Error(`unmapped inlay hint kind '${hint.kind}'; extend LSP_INLAY_KIND`);
        }
        line += `, kind: ${kind}`;
        line += `, label: ${yamlStr(hint.label)}`;
        if (hint.padding_left) {
            line += ", padding_left: true";
        }
        if (hint.padding_right) {
            line += ", padding_right: true";
        }
        out.push(line + " }");
    }
    return out;
};

interface RawDocumentLink {
    range: RawRange;
    target: string;
}

export const renderRawDocumentLinks = (
    result: unknown,
    stripped: Buffer,
    corpus: string,
): string[] => {
    const map = new OffsetConverter(stripped);
    return (result as RawDocumentLink[]).map((link) => {
        const start = map.position(link.range.begin);
        const end = map.position(link.range.end);
        return (
            `- { range: "${start.line}:${start.character}-${end.line}:${end.character}", ` +
            `target: ${yamlStr(normalizeFilePath(link.target, corpus))} }`
        );
    });
};

export interface RawHoverResult {
    range: RawRange | null;
    contents: string;
}

/// Marker payloads ordered like the C++ side emitted them: named markers
/// sorted byte-wise, then `nameless_<i>` numerically.
export function sortedMarkers(markers: Record<string, unknown>): [string, unknown][] {
    const named: string[] = [];
    const nameless: string[] = [];
    for (const key of Object.keys(markers)) {
        (/^nameless_\d+$/.test(key) ? nameless : named).push(key);
    }
    named.sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
    nameless.sort(
        (a, b) => Number(a.slice("nameless_".length)) - Number(b.slice("nameless_".length)),
    );
    return [...named, ...nameless].map((key) => [key, markers[key]]);
}
