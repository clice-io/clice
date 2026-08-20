import * as vscode from "vscode";
import { LanguageClient } from "vscode-languageclient/node";
// Shared protocol shape — type-only, no runtime dependency.
import type { InactiveRegionsParams } from "@clice/tools/protocol" with {
    "resolution-mode": "import",
};

/// Renders clice/inactiveRegions: preprocessor-inactive regions pushed by
/// the server after each compile, dimmed like unreachable code. Switching
/// the compilation context recompiles and re-pushes, so the dimming flips
/// with the selected preprocessor state.
export function registerInactiveRegions(client: LanguageClient, ext: vscode.ExtensionContext) {
    const decoration = vscode.window.createTextEditorDecorationType({
        opacity: "0.45",
    });
    const byUri = new Map<string, vscode.Range[]>();

    function apply(editor: vscode.TextEditor) {
        const ranges = byUri.get(editor.document.uri.toString()) ?? [];
        editor.setDecorations(decoration, ranges);
    }

    client.onNotification("clice/inactiveRegions", (params: InactiveRegionsParams) => {
        const ranges = params.regions.map(
            (r) => new vscode.Range(r.start.line, r.start.character, r.end.line, r.end.character),
        );
        byUri.set(params.uri, ranges);
        for (const editor of vscode.window.visibleTextEditors) {
            if (editor.document.uri.toString() === params.uri) {
                apply(editor);
            }
        }
    });

    ext.subscriptions.push(
        decoration,
        vscode.window.onDidChangeVisibleTextEditors((editors) => {
            editors.forEach(apply);
        }),
    );
}
