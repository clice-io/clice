/// Feature answers as values of the output, from shapes no client owns:
/// `clice inspect` and a server both decode into these, so the two can
/// differ only in data, never in layout.

import { applyEdits, renderDiff, renderWorkspaceEdit, type TextEdit } from "./edits.ts";
import {
    renderLocation,
    type Location,
    type Range,
    type RenderScope,
    type SourceText,
} from "./position.ts";
import { Place, type Table } from "./value.ts";

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

/// A location as the output writes it, its range kept.
export function place(location: Location, scope: RenderScope): Place {
    return new Place(renderLocation(location, scope), location);
}

/// The locations in file and position order.
export function renderLocations(locations: readonly Location[], scope: RenderScope): Place[] {
    return [...locations].sort(compareLocations).map((location) => place(location, scope));
}

export interface Item {
    name: string;
    kind: string;
    /// The item's name; its whole extent renders only when asked.
    selection: Location;
    detail?: string | undefined;
}

export function renderItem(item: Item, scope: RenderScope): Table {
    return { name: item.name, kind: item.kind, at: place(item.selection, scope) };
}

/// A call or type hierarchy item, with the detail an editor shows beside
/// its name when the server sends one.
export function renderHierarchyItem(item: Item, scope: RenderScope): Table {
    return {
        ...renderItem(item, scope),
        ...(item.detail === undefined || item.detail === "" ? {} : { detail: item.detail }),
    };
}

/// Hierarchy items in file and position order.
export function renderHierarchyItems(items: readonly Item[], scope: RenderScope): Table[] {
    return [...items]
        .sort((a, b) => compareLocations(a.selection, b.selection))
        .map((item) => renderHierarchyItem(item, scope));
}

export interface Call {
    item: Item;
    /// Where the calls are, in the caller's file for incoming calls and in
    /// the root's file for outgoing ones.
    sites: Location[];
}

export function renderCalls(calls: readonly Call[], scope: RenderScope): Table[] {
    return [...calls]
        .sort((a, b) => compareLocations(a.item.selection, b.item.selection))
        .map((call) => ({
            ...renderHierarchyItem(call.item, scope),
            sites: renderLocations(call.sites, scope),
        }));
}

export interface Highlight {
    location: Location;
    kind: "text" | "read" | "write";
}

export function renderHighlights(highlights: readonly Highlight[], scope: RenderScope): Table[] {
    return [...highlights]
        .sort((a, b) => compareLocations(a.location, b.location))
        .map((highlight) => ({ at: place(highlight.location, scope), kind: highlight.kind }));
}

export interface Hover {
    /// The card's text, of the markup kind it is written in.
    kind: "markdown" | "plaintext";
    text: string;
    range?: Location | undefined;
}

/// The text as the client receives it, under its markup kind, and the
/// symbol's range; no card is an empty table.
export function renderHover(hover: Hover | null, scope: RenderScope): Table {
    if (hover === null) {
        return {};
    }
    const text = hover.text
        .split("\n")
        .map((line) => line.trimEnd())
        .join("\n");
    return hover.range === undefined
        ? { [hover.kind]: text }
        : { [hover.kind]: text, range: place(hover.range, scope) };
}

export interface Diagnostic {
    location: Location;
    severity: "error" | "warning" | "information" | "hint";
    message: string;
    source?: string | undefined;
    code?: string | undefined;
    /// The notes it carries, as related information.
    related?: { location: Location; message: string }[] | undefined;
    /// Its codeDescription: the page that explains it.
    link?: string | undefined;
    /// Its tags by name (`Unnecessary`, `Deprecated`).
    tags?: string[] | undefined;
}

/// The diagnostics in position order; the parts asked (`related`, `link`,
/// `tags`) join each one, `related` and `tags` even when empty.
export function renderDiagnostics(
    diagnostics: readonly Diagnostic[],
    scope: RenderScope,
    parts: ReadonlySet<string> = new Set(),
): Table[] {
    return [...diagnostics]
        .sort(
            (a, b) => compareLocations(a.location, b.location) || (a.message < b.message ? -1 : 1),
        )
        .map((diagnostic) => {
            const here = anchored(
                scope,
                diagnostic.location.path,
                diagnostic.location.range.start.line,
            );
            return {
                at: place(diagnostic.location, scope),
                severity: diagnostic.severity,
                message: diagnostic.message,
                ...(diagnostic.source === undefined ? {} : { source: diagnostic.source }),
                ...(diagnostic.code === undefined ? {} : { code: diagnostic.code }),
                ...(parts.has("related")
                    ? {
                          related: (diagnostic.related ?? []).map((info) => ({
                              at: place(info.location, here),
                              message: info.message,
                          })),
                      }
                    : {}),
                ...(parts.has("link") && diagnostic.link !== undefined
                    ? { link: diagnostic.link }
                    : {}),
                ...(parts.has("tags") ? { tags: diagnostic.tags ?? [] } : {}),
            };
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

/// What the parts asked show of a hint: its kind, padding and links.
function hintDetails(
    path: string,
    hint: InlayHint,
    parts: ReadonlySet<string>,
    scope: RenderScope,
): Table {
    const here = anchored(scope, path, hint.position.line);
    return {
        label: hint.label,
        ...(parts.has("kind") && hint.kind !== undefined ? { kind: hint.kind } : {}),
        ...(parts.has("padding")
            ? {
                  padding: [
                      ...(hint.paddingLeft === true ? ["left"] : []),
                      ...(hint.paddingRight === true ? ["right"] : []),
                  ],
              }
            : {}),
        ...(parts.has("links")
            ? {
                  links: (hint.links ?? []).map((link) => ({
                      value: link.value,
                      target: place(link.location, here),
                  })),
              }
            : {}),
    };
}

/// Each line carrying hints, with every hint inserted where it shows, in
/// the source's order: `draw(⟨width:⟩ 800)`, a place on the line's text so
/// a reader of the card finds its hints without matching the text. The
/// parts asked (`kind`, `padding`, `links`) list the line's hints with them.
export function renderInlayHints(
    path: string,
    hints: readonly InlayHint[],
    scope: RenderScope,
    parts: ReadonlySet<string> = new Set(),
): Table[] {
    const source = scope.file(path);
    const byLine = new Map<number, InlayHint[]>();
    for (const hint of hints) {
        const list = byLine.get(hint.position.line) ?? [];
        list.push(hint);
        byLine.set(hint.position.line, list);
    }
    const out: Table[] = [];
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
        // The trimmed text starts at the line's first character or at a
        // hint shown in its indentation, whichever comes first.
        const start = Math.min(
            text.length - text.trimStart().length,
            ordered[0]?.hint.position.character ?? text.length,
        );
        out.push({
            line: new Place(rendered.trim(), {
                path,
                range: {
                    start: { line, character: start },
                    end: { line, character: text.trimEnd().length },
                },
            }),
            ...(parts.size === 0
                ? {}
                : { hints: ordered.map(({ hint }) => hintDetails(path, hint, parts, scope)) }),
        });
    }
    return out;
}

export interface Fold {
    range: Range;
    kind: string;
    collapsedText?: string | undefined;
}

export function renderFolds(path: string, folds: readonly Fold[], scope: RenderScope): Table[] {
    return [...folds]
        .sort((a, b) => compareLocations({ path, range: a.range }, { path, range: b.range }))
        .map((fold) => ({
            kind: fold.kind,
            at: place({ path, range: fold.range }, scope),
            ...(fold.collapsedText === undefined ? {} : { collapsed: fold.collapsedText }),
        }));
}

export interface OutlineSymbol extends Item {
    detail?: string | undefined;
    /// The symbol's whole extent.
    range?: Location | undefined;
    /// Its symbol tags by name (`Deprecated`).
    tags?: string[] | undefined;
    children: OutlineSymbol[];
}

/// The symbol tree, each symbol's children under it; the parts asked
/// (`detail`, `range`, `tags`) join each symbol, `tags` even when there
/// are none.
export function renderOutline(
    symbols: readonly OutlineSymbol[],
    scope: RenderScope,
    parts: ReadonlySet<string> = new Set(),
): Table[] {
    return symbols.map((symbol) => {
        const { path, range } = symbol.selection;
        return {
            ...renderItem(symbol, scope),
            ...(parts.has("detail") && symbol.detail !== undefined && symbol.detail !== ""
                ? { detail: symbol.detail }
                : {}),
            ...(parts.has("range") && symbol.range !== undefined
                ? { range: place(symbol.range, anchored(scope, path, range.start.line)) }
                : {}),
            ...(parts.has("tags") ? { tags: symbol.tags ?? [] } : {}),
            ...(symbol.children.length === 0
                ? {}
                : { children: renderOutline(symbol.children, scope, parts) }),
        };
    });
}

/// The ranges around one place, from the innermost out.
export function renderSelection(
    path: string,
    ranges: readonly Range[],
    scope: RenderScope,
): Place[] {
    return ranges.map((range) => place({ path, range }, scope));
}

export interface Link {
    location: Location;
    target: string;
}

export function renderLinks(links: readonly Link[], scope: RenderScope): Table[] {
    return [...links]
        .sort((a, b) => compareLocations(a.location, b.location))
        .map((link) => ({ at: place(link.location, scope), target: link.target }));
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
    /// Ascending is best first: clice writes `8 - score`.
    sortText: string;
    /// The documentation, markdown or plain text as the server sent it.
    documentation?: string | undefined;
    /// The edits elsewhere in the file accepting also makes.
    edits?: { at: Location; text: string }[] | undefined;
}

export interface Completion {
    items: CompletionItem[];
    /// The server sent only some of the items that match: the editor asks
    /// again as the user types instead of filtering these.
    incomplete: boolean;
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

/// The first items by rank, and how many more there are, so a short list
/// never reads as "nothing else matched"; a list the server cut short says
/// so. The parts asked (`insert`, `filter`, `edit`, `documentation`, `edits`)
/// join each item.
export function renderCompletion(
    completion: Completion,
    options: { scope?: RenderScope; parts?: ReadonlySet<string>; limit?: number } = {},
): Table {
    const { items } = completion;
    const { scope, parts = new Set<string>(), limit = 10 } = options;
    const shown = rank(items)
        .slice(0, limit)
        .map((item) => ({
            label: item.label,
            kind: item.kind,
            ...(item.detail === undefined || item.detail === "" ? {} : { detail: item.detail }),
            ...(item.deprecated === true ? { deprecated: true } : {}),
            ...(parts.has("insert")
                ? {
                      insert: item.insert ?? item.label,
                      ...(item.snippet === true ? { snippet: true } : {}),
                  }
                : {}),
            ...(parts.has("filter") && item.filter !== undefined && item.filter !== item.label
                ? { filter: item.filter }
                : {}),
            ...(parts.has("edit") && scope !== undefined && item.replace !== undefined
                ? { replace: place(item.replace, scope) }
                : {}),
            ...(parts.has("edit") && scope !== undefined && item.insertRange !== undefined
                ? { insert_range: place(item.insertRange, scope) }
                : {}),
            ...(parts.has("documentation") && item.documentation !== undefined
                ? { documentation: item.documentation }
                : {}),
            ...(parts.has("edits") && scope !== undefined && item.edits !== undefined
                ? {
                      edits: item.edits.map((extra) => ({
                          at: place(extra.at, scope),
                          text: extra.text,
                      })),
                  }
                : {}),
        }));
    return {
        items: shown,
        ...(items.length > limit ? { more: items.length - limit } : {}),
        ...(completion.incomplete ? { incomplete: true } : {}),
    };
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

/// Each signature with its active parameter in ⟦⟧, and which one is
/// active.
export function renderSignatures({ signatures, active }: Signatures): Table {
    const labels = signatures.map(({ label, active: parameter }) =>
        parameter === undefined
            ? label
            : `${label.slice(0, parameter.begin)}⟦${label.slice(parameter.begin, parameter.end)}⟧${label.slice(parameter.end)}`,
    );
    return labels.length === 0 ? { signatures: [] } : { signatures: labels, active };
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

function describeToken(token: Token): Table {
    return token.modifiers.length === 0
        ? { type: token.type }
        : { type: token.type, modifiers: token.modifiers };
}

export function renderTokens(path: string, tokens: readonly Token[], scope: RenderScope): Table[] {
    return tokens.map((token) => {
        const range = {
            start: { line: token.line, character: token.character },
            end: { line: token.line, character: token.character + token.length },
        };
        return { at: place({ path, range }, scope), ...describeToken(token) };
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
export function renderTokenAt(line: string, character: number, token: Token | null): Table {
    return token === null
        ? { text: wordAt(line, character), type: "none" }
        : {
              text: line.slice(token.character, token.character + token.length),
              ...describeToken(token),
          };
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

/// Each action with what it does to every file it edits, and, checked,
/// whether the result compiles.
export function renderActions(
    actions: readonly Action[],
    file: (path: string) => SourceText,
): Table[] {
    return actions.map((action) => ({
        kind: action.kind,
        title: action.title,
        ...(action.errors === undefined ? {} : { compiles: action.errors.length === 0 }),
        ...(action.edits.size === 0 ? {} : { edits: renderWorkspaceEdit(action.edits, file) }),
        ...(action.errors === undefined || action.errors.length === 0
            ? {}
            : { errors: action.errors }),
        ...(action.host.length === 0 ? {} : { host: action.host }),
    }));
}

export interface WorkspaceSymbol extends Item {
    container?: string | undefined;
}

/// Each symbol with its file, the query having none of its own, in file
/// and position order.
export function renderWorkspaceSymbols(
    symbols: readonly WorkspaceSymbol[],
    scope: RenderScope,
): Table[] {
    return [...symbols]
        .sort(
            (a, b) =>
                compareLocations(a.selection, b.selection) ||
                order(a.name, b.name) ||
                order(a.kind, b.kind) ||
                order(a.container ?? "", b.container ?? ""),
        )
        .map((symbol) => ({
            ...renderItem(symbol, scope),
            ...(symbol.container === undefined ? {} : { container: symbol.container }),
        }));
}

/// The files a file switches to, best first; the preferred one marked.
export interface Counterparts {
    preferred?: string | undefined;
    candidates: { path: string; reasons: string[] }[];
}

export function renderCounterparts({ preferred, candidates }: Counterparts): Table {
    return {
        ...(preferred === undefined ? {} : { preferred }),
        candidates: candidates.map(({ path, reasons }) => ({ path, reasons })),
    };
}

/// The file formatting leaves, as a diff of the text the edits were asked
/// for; a reply of null is no answer at all.
export function renderFormatting(source: SourceText, edits: readonly TextEdit[] | null): string {
    if (edits === null) {
        return "(no answer)";
    }
    return renderDiff(source.text, applyEdits(source, edits));
}

/// A request the server refused, with the message an editor shows.
export interface Refused {
    error: string;
}

/// Where a rename at a place would apply: the name's range and the text the
/// editor offers to edit.
export interface Renameable {
    location: Location;
    placeholder: string;
}

/// The range and placeholder; nothing to rename is an empty table.
export function renderPrepareRename(
    answer: Renameable | Refused | null,
    scope: RenderScope,
): Table {
    if (answer === null) {
        return {};
    }
    if ("error" in answer) {
        return { error: answer.error };
    }
    return { at: place(answer.location, scope), placeholder: answer.placeholder };
}

export interface Renamed {
    edits: Map<string, TextEdit[]>;
    /// The version of the open buffer each file's edits are for; null where
    /// they are for the disk.
    versions: Map<string, number | null>;
    /// What the server showed beside the edits (window/showMessage).
    notice?: string | undefined;
    /// The errors the file renamed at compiles with once the edits apply,
    /// when checked.
    errors?: string[] | undefined;
}

/// Each changed file's diff and the notice beside them, and checked,
/// whether the file renamed at compiles. Part `versions`: the buffer
/// version each file's edits are for, `disk` for a file not open.
export function renderRename(
    answer: Renamed | Refused | null,
    file: (path: string) => SourceText,
    parts: ReadonlySet<string>,
): Table {
    if (answer === null) {
        return {};
    }
    if ("error" in answer) {
        return { error: answer.error };
    }
    return {
        edits: renderWorkspaceEdit(answer.edits, file),
        ...(parts.has("versions")
            ? {
                  versions: Object.fromEntries(
                      [...answer.versions.keys()]
                          .sort()
                          .map((path) => [path, answer.versions.get(path) ?? "disk"]),
                  ),
              }
            : {}),
        ...(answer.notice === undefined ? {} : { notice: answer.notice }),
        ...(answer.errors === undefined ? {} : { compiles: answer.errors.length === 0 }),
        ...(answer.errors === undefined || answer.errors.length === 0
            ? {}
            : { errors: answer.errors }),
    };
}
