import * as vscode from "vscode";
import type { DocumentSemanticsTokensSignature } from "vscode-languageclient/node";
import type { ClientHandle } from "../client";

/// Renders preprocessor-inactive regions dimmed, like unreachable code.
/// The server tags every token inside an inactive region with the
/// `inactive` semantic token modifier (bare identifiers travel as the
/// deliberately unstyled `identifier` type so they have a token to
/// carry it). This middleware decodes the token stream VS Code pulls
/// anyway, merges tagged tokens into whole-line ranges and dims them
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

    /// Inactive regions are whole lines by construction (the server scans
    /// from the line after the opening directive to the line before the
    /// closing one), so tagged tokens merge into line runs: an untagged
    /// token ends a run — it can only sit on an active line — while
    /// token-free gaps inside a region (blank lines, bare punctuation)
    /// bridge it. Expanding each run to full lines then covers those
    /// gaps without ever touching an active line.
    function decode(document: vscode.TextDocument, data: Uint32Array): vscode.Range[] {
        const mask = inactiveMask();
        if (mask === 0) {
            return [];
        }
        const ranges: vscode.Range[] = [];
        // The response may describe a version the buffer already moved
        // past; clamping keeps lineAt in bounds until the re-pull.
        const lastLine = document.lineCount - 1;
        let line = 0;
        let runStart = -1;
        let runEnd = -1;
        const flush = () => {
            if (runStart >= 0 && runStart <= lastLine) {
                const end = Math.min(runEnd, lastLine);
                ranges.push(new vscode.Range(runStart, 0, end, document.lineAt(end).text.length));
            }
            runStart = -1;
        };
        for (let i = 0; i + 4 < data.length; i += 5) {
            line += data[i];
            if ((data[i + 4] & mask) === 0) {
                flush();
                continue;
            }
            if (runStart < 0) {
                runStart = line;
            }
            runEnd = line;
        }
        flush();
        return ranges;
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
        if (tokens) {
            byUri.set(document.uri.toString(), decode(document, tokens.data));
            for (const editor of vscode.window.visibleTextEditors) {
                if (editor.document.uri.toString() === document.uri.toString()) {
                    apply(editor);
                }
            }
        }
        return tokens ?? undefined;
    };
}
