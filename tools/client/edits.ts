/// Apply LSP text edits to a document the way a client does, for tests
/// that assert on the text a code action produces.

import type * as proto from "vscode-languageserver-protocol";

/// Byte offsets of each line start; positions count UTF-16 code units,
/// which equal string indices for the ASCII fixtures this serves.
function offsetOf(text: string, position: proto.Position): number {
    let offset = 0;
    for (let line = 0; line < position.line; line++) {
        const newline = text.indexOf("\n", offset);
        if (newline === -1) {
            return text.length;
        }
        offset = newline + 1;
    }
    return Math.min(offset + position.character, text.length);
}

/// The document after `edits`, each replacing its range of the original
/// text; ranges must not overlap.
export function applyTextEdits(text: string, edits: readonly proto.TextEdit[]): string {
    const ordered = edits
        .map((edit) => ({
            begin: offsetOf(text, edit.range.start),
            end: offsetOf(text, edit.range.end),
            newText: edit.newText,
        }))
        .sort((a, b) => b.begin - a.begin || b.end - a.end);
    let result = text;
    for (const edit of ordered) {
        result = result.slice(0, edit.begin) + edit.newText + result.slice(edit.end);
    }
    return result;
}

/// The text edits a code action applies to `uri`, from its versioned
/// document changes.
export function editsFor(action: proto.CodeAction, uri: string): proto.TextEdit[] {
    const edits: proto.TextEdit[] = [];
    for (const change of action.edit?.documentChanges ?? []) {
        if (!("textDocument" in change) || change.textDocument.uri !== uri) {
            continue;
        }
        for (const edit of change.edits) {
            if ("newText" in edit) {
                edits.push(edit);
            }
        }
    }
    return edits;
}
