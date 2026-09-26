import * as fs from "fs";
import * as vscode from "vscode";

const languages = new Set(["c", "cpp", "cuda-cpp"]);

/// The file a document names, as the server tells files apart: the OS's
/// final path (symlinks, junctions and subst drives followed). Undefined
/// for documents the server does not serve and for files not on disk.
function identity(document: vscode.TextDocument): string | undefined {
    if (document.uri.scheme !== "file" || !languages.has(document.languageId)) {
        return undefined;
    }
    try {
        return fs.realpathSync.native(document.uri.fsPath);
    } catch {
        return undefined;
    }
}

/// The selection an opening request asked for (a definition's range, a
/// terminal link's line): it lands as a selection change after the editor
/// turns active, or not at all when none was asked for.
function requestedSelection(editor: vscode.TextEditor): Promise<vscode.Selection> {
    return new Promise((resolve) => {
        const done = (selection: vscode.Selection) => {
            subscription.dispose();
            clearTimeout(timer);
            resolve(selection);
        };
        const subscription = vscode.window.onDidChangeTextEditorSelection((event) => {
            if (event.textEditor === editor) {
                done(event.selections[0] ?? editor.selection);
            }
        });
        const timer = setTimeout(() => {
            done(editor.selection);
        }, 200);
    });
}

/// VS Code opens a file reached through a symlink, a junction or a subst
/// drive as a second document with its own buffer; the server answers one
/// buffer per file. An editor showing such a second name is replaced by the
/// one already open, at the same selection.
async function redirect(editor: vscode.TextEditor | undefined) {
    if (!editor) {
        return;
    }
    const opened = editor.document;
    const file = identity(opened);
    if (!file) {
        return;
    }
    const first = vscode.workspace.textDocuments.find(
        (document) =>
            document !== opened &&
            document.uri.toString() !== opened.uri.toString() &&
            identity(document) === file,
    );
    if (!first) {
        return;
    }
    const tab = vscode.window.tabGroups.all
        .flatMap((group) => group.tabs)
        .find(
            (candidate) =>
                candidate.input instanceof vscode.TabInputText &&
                candidate.input.uri.toString() === opened.uri.toString(),
        );
    const selection = await requestedSelection(editor);
    const viewColumn = editor.viewColumn;
    const shown = await vscode.window.showTextDocument(first, { viewColumn, preview: false });
    shown.selection = selection;
    shown.revealRange(selection);
    if (tab) {
        await vscode.window.tabGroups.close(tab);
    }
}

export function registerAliasRedirect(context: vscode.ExtensionContext) {
    context.subscriptions.push(
        vscode.window.onDidChangeActiveTextEditor((editor) => void redirect(editor)),
    );
}
