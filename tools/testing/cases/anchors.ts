/// Places in a workspace named by their text: `at("src/circle.cpp",
/// "double ar|ea")` is where that snippet occurs, the cursor at `|` (its
/// start without one). A snippet must occur exactly once in the file, so a
/// file can grow around it without moving it.

export type Anchor = TextAnchor | MarkerAnchor;

export interface TextAnchor {
    readonly path: string;
    readonly anchor: string;
}

/// A marker of the case's sources: UTF-16 offsets into the file as the
/// case writes it, valid while the file reads so.
export interface MarkerAnchor {
    readonly path: string;
    /// `§(name)`, or `nameless_<i>` for the i-th unnamed marker of the case.
    readonly name: string;
    readonly begin: number;
    readonly end: number;
    /// A `§⟦...⟧` range rather than a `§` point.
    readonly range: boolean;
    /// The file's text the offsets count in.
    readonly text: string;
}

export function at(path: string, anchor: string): TextAnchor {
    return { path, anchor };
}

export function describeAnchor(anchor: Anchor): string {
    return "name" in anchor
        ? `${anchor.path} at §${anchor.name}`
        : `${anchor.path} at ${JSON.stringify(anchor.anchor)}`;
}

function unmoved(text: string, anchor: MarkerAnchor): void {
    if (text !== anchor.text) {
        throw new Error(
            `${describeAnchor(anchor)}: ${anchor.path} changed since the case wrote it; name the place with at()`,
        );
    }
}

/// The span an anchor covers: a marker's range, or a snippet's text.
export function spanOf(text: string, anchor: Anchor): { begin: number; end: number } {
    if ("name" in anchor) {
        unmoved(text, anchor);
        return { begin: anchor.begin, end: anchor.end };
    }
    return uniqueSpan(text, snippetOf(anchor.anchor).snippet, anchor.path);
}

/// Where the one occurrence of `snippet` lies in `text`; an error naming
/// `where` when it occurs never or more than once.
function uniqueSpan(text: string, snippet: string, where: string): { begin: number; end: number } {
    const begin = text.indexOf(snippet);
    if (snippet === "" || begin < 0) {
        throw new Error(`${where} has no ${JSON.stringify(snippet)}`);
    }
    if (text.includes(snippet, begin + 1)) {
        throw new Error(`${where} has ${JSON.stringify(snippet)} more than once`);
    }
    return { begin, end: begin + snippet.length };
}

/// The snippet an anchor stands for and where its cursor is in it.
function snippetOf(anchor: string): { snippet: string; cursor: number } {
    const bar = anchor.indexOf("|");
    if (bar >= 0 && anchor.includes("|", bar + 1)) {
        throw new Error(`anchor ${JSON.stringify(anchor)} has more than one cursor`);
    }
    return bar < 0
        ? { snippet: anchor, cursor: 0 }
        : { snippet: anchor.slice(0, bar) + anchor.slice(bar + 1), cursor: bar };
}

/// The UTF-16 offset of `anchor`'s cursor in `text`.
export function locate(text: string, anchor: Anchor): number {
    if ("name" in anchor) {
        unmoved(text, anchor);
        return anchor.begin;
    }
    const { snippet, cursor } = snippetOf(anchor.anchor);
    return uniqueSpan(text, snippet, anchor.path).begin + cursor;
}

/// A change to a text by the unique snippets it touches; `text` replaces
/// the whole of it.
export type Change =
    | { replace: string; with: string }
    | { after: string; insert: string }
    | { before: string; insert: string }
    | { remove: string }
    | { text: string };

export function applyChange(text: string, change: Change, where: string): string {
    if ("text" in change) {
        return change.text;
    }
    if ("replace" in change) {
        const { begin, end } = uniqueSpan(text, change.replace, where);
        return text.slice(0, begin) + change.with + text.slice(end);
    }
    if ("after" in change) {
        const { end } = uniqueSpan(text, change.after, where);
        return text.slice(0, end) + change.insert + text.slice(end);
    }
    if ("before" in change) {
        const { begin } = uniqueSpan(text, change.before, where);
        return text.slice(0, begin) + change.insert + text.slice(begin);
    }
    const { begin, end } = uniqueSpan(text, change.remove, where);
    return text.slice(0, begin) + text.slice(end);
}
