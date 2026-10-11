/// Feature answers rendered as text, from shapes no client owns: `clice
/// inspect` and a server both decode into these, so the two can differ only
/// in data, never in layout.

import { renderWorkspaceEdit, type TextEdit } from "./edits.ts";
import {
    renderLocation,
    type Location,
    type Range,
    type RenderScope,
    type SourceText,
} from "./position.ts";

/// `scope` with same-file offsets counted from `line` of `path`: a detail
/// rendered under an item reads relative to the item.
function anchored(scope: RenderScope, path: string, line: number): RenderScope {
    return scope.query?.path === path ? { ...scope, query: { path, line } } : scope;
}

export function compareLocations(a: Location, b: Location): number {
    return (
        (a.path < b.path ? -1 : a.path > b.path ? 1 : 0) ||
        a.range.start.line - b.range.start.line ||
        a.range.start.character - b.range.start.character ||
        a.range.end.line - b.range.end.line ||
        a.range.end.character - b.range.end.character
    );
}

/// One location a line, in file and position order.
export function renderLocations(locations: readonly Location[], scope: RenderScope): string[] {
    return [...locations].sort(compareLocations).map((location) => renderLocation(location, scope));
}

export interface Item {
    name: string;
    kind: string;
    /// The item's name; its whole extent renders only when asked.
    selection: Location;
}

export function renderItem(item: Item, scope: RenderScope): string {
    return `${item.name} (${item.kind}) ${renderLocation(item.selection, scope)}`;
}

export interface Call {
    item: Item;
    /// Where the calls are, in the caller's file for incoming calls and in
    /// the root's file for outgoing ones.
    sites: Location[];
}

export function renderCalls(calls: readonly Call[], scope: RenderScope): string[] {
    const out: string[] = [];
    for (const call of [...calls].sort((a, b) =>
        compareLocations(a.item.selection, b.item.selection),
    )) {
        out.push(renderItem(call.item, scope));
        for (const site of renderLocations(call.sites, scope)) {
            out.push(`  at ${site}`);
        }
    }
    return out;
}

export interface Highlight {
    location: Location;
    kind: "text" | "read" | "write";
}

export function renderHighlights(highlights: readonly Highlight[], scope: RenderScope): string[] {
    return [...highlights]
        .sort((a, b) => compareLocations(a.location, b.location))
        .map((highlight) => `${renderLocation(highlight.location, scope)}  ${highlight.kind}`);
}

export interface Hover {
    markdown: string;
    range?: Location | undefined;
}

export function renderHover(hover: Hover | null, scope: RenderScope): string[] {
    if (hover === null) {
        return ["(none)"];
    }
    const out = hover.markdown.split("\n").map((line) => line.trimEnd());
    if (hover.range !== undefined) {
        out.push(`range: ${renderLocation(hover.range, scope)}`);
    }
    return out;
}

export interface Diagnostic {
    location: Location;
    severity: "error" | "warning" | "information" | "hint";
    message: string;
    source?: string | undefined;
    code?: string | undefined;
}

export function renderDiagnostics(
    diagnostics: readonly Diagnostic[],
    scope: RenderScope,
): string[] {
    return [...diagnostics]
        .sort(
            (a, b) => compareLocations(a.location, b.location) || (a.message < b.message ? -1 : 1),
        )
        .map((diagnostic) => {
            const origin = [diagnostic.source, diagnostic.code].filter(
                (part) => part !== undefined,
            );
            const tail = origin.length > 0 ? ` [${origin.join(" ")}]` : "";
            return `${renderLocation(diagnostic.location, scope)}  ${diagnostic.severity}: ${diagnostic.message}${tail}`;
        });
}

export interface InlayHint {
    position: { line: number; character: number };
    label: string;
    kind?: "Type" | "Parameter" | undefined;
    paddingLeft?: boolean | undefined;
    paddingRight?: boolean | undefined;
    /// The label pieces that lead somewhere.
    links?: { value: string; location: Location }[] | undefined;
}

function hintDetails(
    path: string,
    hint: InlayHint,
    parts: ReadonlySet<string>,
    scope: RenderScope,
): string[] {
    const here = anchored(scope, path, hint.position.line);
    const fields: string[] = [];
    if (parts.has("kind") && hint.kind !== undefined) {
        fields.push(hint.kind);
    }
    if (parts.has("padding")) {
        const sides = [hint.paddingLeft ? "left" : "", hint.paddingRight ? "right" : ""].filter(
            (side) => side !== "",
        );
        fields.push(sides.length === 0 ? "no padding" : `padding ${sides.join(" ")}`);
    }
    const out = fields.length === 0 ? [] : [`  ⟨${hint.label}⟩ ${fields.join(", ")}`];
    if (parts.has("links")) {
        for (const link of hint.links ?? []) {
            out.push(`  ⟨${hint.label}⟩ ${link.value} → ${renderLocation(link.location, here)}`);
        }
    }
    return out;
}

/// Each line carrying hints, with every hint inserted where it shows, in
/// the source's order: `draw(⟨width:⟩ 800)`. The parts asked (`kind`,
/// `padding`, `links`) add a line per hint below it.
export function renderInlayHints(
    path: string,
    hints: readonly InlayHint[],
    scope: RenderScope,
    parts: ReadonlySet<string> = new Set(),
): string[] {
    const source = scope.file(path);
    const byLine = new Map<number, InlayHint[]>();
    for (const hint of hints) {
        const list = byLine.get(hint.position.line) ?? [];
        list.push(hint);
        byLine.set(hint.position.line, list);
    }
    const out: string[] = [];
    for (const line of [...byLine.keys()].sort((a, b) => a - b)) {
        const text = source.line(line);
        const ordered = (byLine.get(line) ?? [])
            .map((hint, index) => ({ hint, index }))
            .sort(
                (a, b) =>
                    a.hint.position.character - b.hint.position.character || a.index - b.index,
            );
        let rendered = "";
        let at = 0;
        for (const { hint } of ordered) {
            rendered += text.slice(at, hint.position.character) + `⟨${hint.label}⟩`;
            at = hint.position.character;
        }
        rendered += text.slice(at);
        out.push(rendered.trim());
        for (const { hint } of ordered) {
            out.push(...hintDetails(path, hint, parts, scope));
        }
    }
    return out;
}

export interface Fold {
    range: Range;
    kind: string;
    collapsedText?: string | undefined;
}

export function renderFolds(path: string, folds: readonly Fold[], scope: RenderScope): string[] {
    return [...folds]
        .sort((a, b) => compareLocations({ path, range: a.range }, { path, range: b.range }))
        .map((fold) => {
            const collapsed = fold.collapsedText === undefined ? "" : `  "${fold.collapsedText}"`;
            return `${fold.kind}: ${renderLocation({ path, range: fold.range }, scope)}${collapsed}`;
        });
}

export interface OutlineSymbol extends Item {
    detail?: string | undefined;
    /// The symbol's whole extent.
    range?: Location | undefined;
    children: OutlineSymbol[];
}

/// The symbol tree, a child indented under its parent; the parts asked
/// (`detail`, `range`) follow on each symbol's line.
export function renderOutline(
    symbols: readonly OutlineSymbol[],
    scope: RenderScope,
    parts: ReadonlySet<string> = new Set(),
    depth = 0,
): string[] {
    const out: string[] = [];
    for (const symbol of symbols) {
        let line = `${"  ".repeat(depth)}${renderItem(symbol, scope)}`;
        if (parts.has("detail") && symbol.detail !== undefined && symbol.detail !== "") {
            line += `  detail: ${symbol.detail}`;
        }
        if (parts.has("range") && symbol.range !== undefined) {
            const { path, range } = symbol.selection;
            line += `  range: ${renderLocation(symbol.range, anchored(scope, path, range.start.line))}`;
        }
        out.push(line);
        out.push(...renderOutline(symbol.children, scope, parts, depth + 1));
    }
    return out;
}

/// The ranges around one place, from the innermost out.
export function renderSelection(
    path: string,
    ranges: readonly Range[],
    scope: RenderScope,
): string[] {
    return ranges.map((range) => renderLocation({ path, range }, scope));
}

export interface Link {
    location: Location;
    target: string;
}

export function renderLinks(links: readonly Link[], scope: RenderScope): string[] {
    return [...links]
        .sort((a, b) => compareLocations(a.location, b.location))
        .map((link) => `${renderLocation(link.location, scope)} → ${link.target}`);
}

export interface CompletionItem {
    label: string;
    kind: string;
    detail?: string | undefined;
    deprecated?: boolean | undefined;
    filter?: string | undefined;
    /// The text accepting the item inserts: its edit's, else its insert text.
    insert?: string | undefined;
    snippet?: boolean | undefined;
    /// The range its edit replaces, and the shorter one an insert/replace
    /// edit inserts over.
    replace?: Location | undefined;
    insertRange?: Location | undefined;
    /// Ascending is best first: clice writes `2 - score`.
    sortText: string;
}

function order(a: string, b: string): number {
    return a < b ? -1 : a > b ? 1 : 0;
}

/// Items arrive in no defined order, so they rank as a client ranks them,
/// ties falling to the label and then the rest of the line: the same order
/// on every host.
function rank(items: readonly CompletionItem[]): CompletionItem[] {
    return [...items].sort(
        (a, b) =>
            order(a.sortText, b.sortText) ||
            order(a.label, b.label) ||
            order(a.kind, b.kind) ||
            order(a.detail ?? "", b.detail ?? ""),
    );
}

/// The first items by rank; the cut is announced so a short list never
/// reads as "nothing else matched". The parts asked (`insert`, `filter`,
/// `edit`) add lines below each item.
export function renderCompletion(
    items: readonly CompletionItem[],
    options: { scope?: RenderScope; parts?: ReadonlySet<string>; limit?: number } = {},
): string[] {
    if (items.length === 0) {
        return ["(none)"];
    }
    const { scope, parts = new Set<string>(), limit = 10 } = options;
    const out: string[] = [];
    for (const item of rank(items).slice(0, limit)) {
        const detail = item.detail === undefined || item.detail === "" ? "" : `  ${item.detail}`;
        out.push(`${item.label}  (${item.kind})${detail}${item.deprecated ? "  deprecated" : ""}`);
        if (parts.has("insert")) {
            const text = JSON.stringify(item.insert ?? item.label);
            out.push(`  inserts ${text}${item.snippet ? " as a snippet" : ""}`);
        }
        if (parts.has("filter") && item.filter !== undefined && item.filter !== item.label) {
            out.push(`  filters by ${JSON.stringify(item.filter)}`);
        }
        if (parts.has("edit") && scope !== undefined) {
            if (item.replace !== undefined) {
                out.push(`  replaces ${renderLocation(item.replace, scope)}`);
            }
            if (item.insertRange !== undefined) {
                out.push(`  inserts over ${renderLocation(item.insertRange, scope)}`);
            }
        }
    }
    if (items.length > limit) {
        out.push(`… +${items.length - limit} more`);
    }
    return out;
}

export interface Signature {
    label: string;
    /// The active parameter's span in the label.
    active?: { begin: number; end: number } | undefined;
}

export interface Signatures {
    signatures: Signature[];
    active: number;
}

export function renderSignatures({ signatures, active: activeSignature }: Signatures): string[] {
    return signatures.map((signature, index) => {
        const { label, active } = signature;
        const text =
            active === undefined
                ? label
                : `${label.slice(0, active.begin)}⟦${label.slice(active.begin, active.end)}⟧${label.slice(active.end)}`;
        return `${index === activeSignature ? "▶" : " "} ${text}`;
    });
}

/// A semantic token on one line, as the server sends it: a multiline token
/// comes as one piece per line.
export interface Token {
    line: number;
    character: number;
    length: number;
    type: string;
    modifiers: string[];
}

function describeToken(token: Token): string {
    return token.modifiers.length === 0
        ? token.type
        : `${token.type} [${token.modifiers.join(", ")}]`;
}

export function renderTokens(path: string, tokens: readonly Token[], scope: RenderScope): string[] {
    return tokens.map((token) => {
        const range = {
            start: { line: token.line, character: token.character },
            end: { line: token.line, character: token.character + token.length },
        };
        return `${describeToken(token)}: ${renderLocation({ path, range }, scope)}`;
    });
}

const WORD = /\w/;

/// The identifier at `character`, or the one character there: what a
/// place no token covers points at.
function wordAt(line: string, character: number): string {
    let end = character;
    while (end < line.length && WORD.test(line.charAt(end))) {
        end += 1;
    }
    if (end === character) {
        const point = line.codePointAt(character);
        return point === undefined ? "" : String.fromCodePoint(point);
    }
    let begin = character;
    while (begin > 0 && WORD.test(line.charAt(begin - 1))) {
        begin -= 1;
    }
    return line.slice(begin, end);
}

/// The token a place lies in, as an editor's token inspector shows it:
/// its text and what it is, or the word there and `none`.
export function renderTokenAt(line: string, character: number, token: Token | null): string {
    return token === null
        ? `${wordAt(line, character)}  none`
        : `${line.slice(token.character, token.character + token.length)}  ${describeToken(token)}`;
}

export interface Action {
    kind: string;
    title: string;
    edits: Map<string, TextEdit[]>;
    /// What a request to define into the host source would place, where the
    /// client has no host to resolve it in.
    host: string[];
    /// The errors the file compiles with once the edits apply, for an action
    /// checked so.
    errors?: string[] | undefined;
}

function indent(line: string): string {
    return line === "" ? "" : `  ${line}`;
}

export function renderActions(
    actions: readonly Action[],
    file: (path: string) => SourceText,
): string[] {
    return actions.flatMap((action) => {
        const outcome =
            action.errors === undefined
                ? ""
                : action.errors.length === 0
                  ? " → compiles"
                  : " → does not compile";
        return [
            `[${action.kind}] ${action.title}${outcome}`,
            ...renderWorkspaceEdit(action.edits, file).map(indent),
            ...(action.errors ?? []).map((error) => `  error: ${error}`),
            ...(action.host.length === 0
                ? []
                : [
                      "  into the host source:",
                      ...action.host.flatMap((piece) =>
                          piece.split("\n").map((line) => indent(indent(line))),
                      ),
                  ]),
        ];
    });
}

export interface WorkspaceSymbol extends Item {
    container?: string | undefined;
}

/// Each symbol with its file, the query having none of its own, in file
/// and position order.
export function renderWorkspaceSymbols(
    symbols: readonly WorkspaceSymbol[],
    scope: RenderScope,
): string[] {
    const lines = symbols.map((symbol) => ({
        symbol,
        line: `${renderItem(symbol, scope)}${symbol.container === undefined ? "" : `  in ${symbol.container}`}`,
    }));
    lines.sort(
        (a, b) => compareLocations(a.symbol.selection, b.symbol.selection) || order(a.line, b.line),
    );
    return lines.map(({ line }) => line);
}

/// The files a file switches to, best first; the preferred one marked.
export interface Counterparts {
    preferred?: string | undefined;
    candidates: { path: string; reasons: string[] }[];
}

export function renderCounterparts({ preferred, candidates }: Counterparts): string[] {
    return candidates.map(
        ({ path, reasons }) => `${path === preferred ? "▶" : " "} ${path}  (${reasons.join(", ")})`,
    );
}
