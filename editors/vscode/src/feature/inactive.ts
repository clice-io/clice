import * as vscode from "vscode";
import type { DocumentSemanticsTokensSignature } from "vscode-languageclient/node";
import type { ClientHandle } from "../client";

/// Line runs `[startLine, endLine]` of tokens carrying `mask`, merged in
/// stream order. Inactive regions are whole lines by construction (the
/// server scans from the line after the opening directive to the line
/// before the closing one), so an untagged token — which can only sit on
/// an active line — ends a run, while token-free gaps inside a region
/// (blank lines, bare punctuation) bridge it. Exported for the e2e suite.
export function inactiveRuns(data: ArrayLike<number>, mask: number): [number, number][] {
    const runs: [number, number][] = [];
    let line = 0;
    let runStart = -1;
    let runEnd = -1;
    const flush = () => {
        if (runStart >= 0) {
            runs.push([runStart, runEnd]);
        }
        runStart = -1;
    };
    for (let i = 0; i + 4 < data.length; i += 5) {
        line += data[i] ?? 0;
        if (((data[i + 4] ?? 0) & mask) === 0) {
            flush();
            continue;
        }
        if (runStart < 0) {
            runStart = line;
        }
        runEnd = line;
    }
    flush();
    return runs;
}

/// Renders preprocessor-inactive regions dimmed, like unreachable code.
/// The server tags every token inside an inactive region with the
/// `inactive` semantic token modifier (bare identifiers travel as the
/// deliberately unstyled `identifier` type so they have a token to
/// carry it). This middleware decodes the token stream VS Code pulls
/// anyway, expands the tagged runs to whole-line ranges and dims them
/// with an opacity decoration — TextMate keeps the syntax colors
/// underneath. Recompiles that move regions without an edit (context
/// switches, changed headers) arrive through the server's
/// workspace/semanticTokens/refresh, which re-pulls through this path.
export function registerInactiveRegions(
    client: () => ClientHandle | undefined,
    ext: vscode.ExtensionContext,
): (
    document: vscode.TextDocument,
    token: vscode.CancellationToken,
    next: DocumentSemanticsTokensSignature,
) => Promise<vscode.SemanticTokens | null | undefined> {
    const decoration = vscode.window.createTextEditorDecorationType({
        opacity: "0.45",
    });
    const byUri = new Map<string, vscode.Range[]>();

    function apply(editor: vscode.TextEditor) {
        const ranges = byUri.get(editor.document.uri.toString()) ?? [];
        editor.setDecorations(decoration, ranges);
    }

    function inactiveMask(): number {
        const provider = client()?.current.initializeResult?.capabilities.semanticTokensProvider;
        const index = provider?.legend.tokenModifiers.indexOf("inactive") ?? -1;
        return index < 0 ? 0 : 1 << index;
    }

    function dimRanges(
        document: vscode.TextDocument,
        tokens: vscode.SemanticTokens | null | undefined,
    ): vscode.Range[] {
        const mask = inactiveMask();
        if (!tokens || mask === 0) {
            return [];
        }
        // The response may describe a version the buffer already moved
        // past; clamping keeps lineAt in bounds until the re-pull.
        const lastLine = document.lineCount - 1;
        return inactiveRuns(tokens.data, mask)
            .filter(([start]) => start <= lastLine)
            .map(([start, end]) => {
                const clamped = Math.min(end, lastLine);
                return new vscode.Range(start, 0, clamped, document.lineAt(clamped).text.length);
            });
    }

    ext.subscriptions.push(
        decoration,
        vscode.window.onDidChangeVisibleTextEditors((editors) => {
            editors.forEach(apply);
        }),
        vscode.workspace.onDidCloseTextDocument((document) => {
            byUri.delete(document.uri.toString());
        }),
    );

    return async (document, token, next) => {
        const tokens = await next(document, token);
        // A cancelled request says nothing about the document; any other
        // response is authoritative — including null, which must clear
        // stale dimming instead of leaving it applied indefinitely.
        if (!token.isCancellationRequested) {
            byUri.set(document.uri.toString(), dimRanges(document, tokens));
            for (const editor of vscode.window.visibleTextEditors) {
                if (editor.document.uri.toString() === document.uri.toString()) {
                    apply(editor);
                }
            }
        }
        return tokens ?? undefined;
    };
}
