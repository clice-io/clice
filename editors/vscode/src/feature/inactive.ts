import * as vscode from "vscode";
import type {
    DocumentSemanticsTokensEditsSignature,
    DocumentSemanticsTokensSignature,
} from "vscode-languageclient/node";
import type { ClientHandle } from "../client";

/// What the `inactive` modifier dims in a token stream: runs
/// `[startLine, endLine]` of whole lines, and `[line, character, length]`
/// of the tagged tokens on a line that also runs code — the arguments a
/// macro drops. Inactive regions are whole lines by construction (the
/// server scans from the line after the opening directive to the line
/// before the closing one), so a line holding an untagged token ends a
/// run, while token-free gaps inside a region (blank lines) bridge it.
/// Exported for the e2e suite.
export interface InactiveSpans {
    lines: [number, number][];
    tokens: [number, number, number][];
}

export function inactiveSpans(data: ArrayLike<number>, mask: number): InactiveSpans {
    const spans: InactiveSpans = { lines: [], tokens: [] };
    let runStart = -1;
    let runEnd = -1;
    const flush = () => {
        if (runStart >= 0) {
            spans.lines.push([runStart, runEnd]);
        }
        runStart = -1;
    };

    // The line being read: its tagged tokens, and whether it runs code.
    let current = -1;
    let tagged: [number, number, number][] = [];
    let active = false;
    const endLine = () => {
        if (active) {
            flush();
            spans.tokens.push(...tagged);
        } else if (tagged.length > 0) {
            if (runStart < 0) {
                runStart = current;
            }
            runEnd = current;
        }
        tagged = [];
        active = false;
    };

    let line = 0;
    let character = 0;
    for (let i = 0; i + 4 < data.length; i += 5) {
        const deltaLine = data[i] ?? 0;
        const deltaStart = data[i + 1] ?? 0;
        line += deltaLine;
        character = deltaLine === 0 ? character + deltaStart : deltaStart;
        if (line !== current) {
            endLine();
            current = line;
        }
        if (((data[i + 4] ?? 0) & mask) === 0) {
            active = true;
        } else {
            tagged.push([line, character, data[i + 2] ?? 0]);
        }
    }
    endLine();
    flush();
    return spans;
}

/// The token data an edits reply turns `data` into.
function applyEdits(
    data: ArrayLike<number>,
    edits: readonly vscode.SemanticTokensEdit[],
): number[] {
    const result = Array.from(data);
    for (const edit of [...edits].sort((a, b) => b.start - a.start)) {
        result.splice(edit.start, edit.deleteCount, ...(edit.data ?? []));
    }
    return result;
}

/// Renders preprocessor-inactive regions dimmed, like unreachable code.
/// The server tags every token inside an inactive region with the
/// `inactive` semantic token modifier (bare identifiers travel as the
/// deliberately unstyled `plain` type so they have a token to
/// carry it). This middleware decodes the token stream VS Code pulls
/// anyway — whole, or as edits to the stream it holds — expands the
/// tagged runs to whole-line ranges and dims them with an opacity
/// decoration; TextMate keeps the syntax colors underneath. Recompiles
/// that move regions without an edit (context switches, changed headers)
/// arrive through the server's workspace/semanticTokens/refresh, which
/// re-pulls through this path.
export function registerInactiveRegions(
    client: () => ClientHandle | undefined,
    ext: vscode.ExtensionContext,
): {
    full: (
        document: vscode.TextDocument,
        token: vscode.CancellationToken,
        next: DocumentSemanticsTokensSignature,
    ) => Promise<vscode.SemanticTokens | null | undefined>;
    edits: (
        document: vscode.TextDocument,
        previousResultId: string,
        token: vscode.CancellationToken,
        next: DocumentSemanticsTokensEditsSignature,
    ) => Promise<vscode.SemanticTokens | vscode.SemanticTokensEdits | null | undefined>;
} {
    const decoration = vscode.window.createTextEditorDecorationType({
        opacity: "0.45",
    });
    const byUri = new Map<string, vscode.Range[]>();
    // The stream each document's last reply left, which the next edits
    // reply patches.
    const streams = new Map<string, { resultId: string | undefined; data: ArrayLike<number> }>();

    function apply(editor: vscode.TextEditor) {
        const ranges = byUri.get(editor.document.uri.toString()) ?? [];
        editor.setDecorations(decoration, ranges);
    }

    function inactiveMask(): number {
        const provider = client()?.current.initializeResult?.capabilities.semanticTokensProvider;
        const index = provider?.legend.tokenModifiers.indexOf("inactive") ?? -1;
        return index < 0 ? 0 : 1 << index;
    }

    function dimRanges(document: vscode.TextDocument, data: ArrayLike<number>): vscode.Range[] {
        const mask = inactiveMask();
        if (mask === 0) {
            return [];
        }
        // The response may describe a version the buffer already moved
        // past; clamping keeps lineAt in bounds until the re-pull.
        const lastLine = document.lineCount - 1;
        const spans = inactiveSpans(data, mask);
        const lines = spans.lines
            .filter(([start]) => start <= lastLine)
            .map(([start, end]) => {
                const clamped = Math.min(end, lastLine);
                return new vscode.Range(start, 0, clamped, document.lineAt(clamped).text.length);
            });
        const tokens = spans.tokens
            .filter(([line]) => line <= lastLine)
            .map(
                ([line, character, length]) =>
                    new vscode.Range(line, character, line, character + length),
            );
        return [...lines, ...tokens];
    }

    function show(
        document: vscode.TextDocument,
        stream: { resultId: string | undefined; data: ArrayLike<number> } | undefined,
    ) {
        const uri = document.uri.toString();
        if (stream) {
            streams.set(uri, stream);
        } else {
            streams.delete(uri);
        }
        byUri.set(uri, stream ? dimRanges(document, stream.data) : []);
        for (const editor of vscode.window.visibleTextEditors) {
            if (editor.document.uri.toString() === uri) {
                apply(editor);
            }
        }
    }

    ext.subscriptions.push(
        decoration,
        vscode.window.onDidChangeVisibleTextEditors((editors) => {
            editors.forEach(apply);
        }),
        vscode.workspace.onDidCloseTextDocument((document) => {
            byUri.delete(document.uri.toString());
            streams.delete(document.uri.toString());
        }),
    );

    // A cancelled request says nothing about the document; any other
    // response is authoritative — including null, which must clear stale
    // dimming instead of leaving it applied indefinitely.
    return {
        full: async (document, token, next) => {
            const tokens = await next(document, token);
            if (!token.isCancellationRequested) {
                show(
                    document,
                    tokens ? { resultId: tokens.resultId, data: tokens.data } : undefined,
                );
            }
            return tokens ?? undefined;
        },
        edits: async (document, previousResultId, token, next) => {
            const reply = await next(document, previousResultId, token);
            if (!token.isCancellationRequested) {
                const base = streams.get(document.uri.toString());
                if (!reply) {
                    show(document, undefined);
                } else if ("data" in reply) {
                    show(document, { resultId: reply.resultId, data: reply.data });
                } else if (base?.resultId === previousResultId) {
                    show(document, {
                        resultId: reply.resultId,
                        data: applyEdits(base.data, reply.edits),
                    });
                }
            }
            return reply ?? undefined;
        },
    };
}
