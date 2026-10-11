/// Edits rendered as what they do: the files after applying them, as a
/// diff with two lines of context and no line numbers. Whitespace a line
/// ends with shows (`·` a space, `→` a tab), and so does a last line with
/// no newline, so no change to either disappears from the rendering.

import { structuredPatch } from "diff";
import type { Range, SourceText } from "./position.ts";
import type { Table } from "./value.ts";

export interface TextEdit {
    range: Range;
    newText: string;
}

/// Apply non-overlapping edits; edits starting at the same place apply in
/// the order given, as LSP requires.
export function applyEdits(source: SourceText, edits: readonly TextEdit[]): string {
    const ordered = edits
        .map((edit, index) => ({
            start: source.offsetAt(edit.range.start),
            end: source.offsetAt(edit.range.end),
            text: edit.newText,
            index,
        }))
        .sort((a, b) => b.start - a.start || b.index - a.index);
    let text = source.text;
    let limit = Number.POSITIVE_INFINITY;
    for (const edit of ordered) {
        if (edit.end > limit) {
            throw new Error(`${source.path}: overlapping edits`);
        }
        text = text.slice(0, edit.start) + edit.text + text.slice(edit.end);
        limit = edit.start;
    }
    return text;
}

function visibleEnd(line: string): string {
    return line.replace(/[ \t]+$/, (run) => run.replaceAll(" ", "·").replaceAll("\t", "→"));
}

/// The diff from `before` to `after` of a file.
export function renderDiff(path: string, before: string, after: string): string {
    const patch = structuredPatch(path, path, before, after, undefined, undefined, { context: 2 });
    const out: string[] = [];
    patch.hunks.forEach((hunk, index) => {
        if (index > 0) {
            out.push("…");
        }
        for (const line of hunk.lines) {
            // An empty context line stays empty rather than end in its space.
            out.push(
                line === " " || line.startsWith("\\")
                    ? line.trim()
                    : line.charAt(0) + visibleEnd(line.slice(1)),
            );
        }
    });
    if (patch.hunks.length === 0) {
        out.push("(no change)");
    }
    return out.join("\n");
}

/// The diff of every file a workspace edit touches, by file.
export function renderWorkspaceEdit(
    edits: ReadonlyMap<string, readonly TextEdit[]>,
    file: (path: string) => SourceText,
): Table {
    return Object.fromEntries(
        [...edits.keys()].sort().map((path) => {
            const source = file(path);
            return [path, renderDiff(path, source.text, applyEdits(source, edits.get(path) ?? []))];
        }),
    );
}
