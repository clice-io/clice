import * as vscode from "vscode";
import type { HandleDiagnosticsSignature } from "vscode-languageclient/node";
import type { ClientHandle } from "../client";

/// The files an editor or a tab shows. A tab counts as vscode-languageclient
/// counts it for the diagnostics it pulls; the editors cover the tabs the API
/// cannot read, the merge editor's and the multi-file diff's.
function shownFiles(): Set<string> {
    const uris = new Set(
        vscode.window.visibleTextEditors.map((editor) => editor.document.uri.toString()),
    );
    for (const group of vscode.window.tabGroups.all) {
        for (const tab of group.tabs) {
            const input = tab.input;
            if (input instanceof vscode.TabInputText || input instanceof vscode.TabInputCustom) {
                uris.add(input.uri.toString());
            } else if (input instanceof vscode.TabInputTextDiff) {
                uris.add(input.modified.toString());
            }
        }
    }
    return uris;
}

/// Problems lists what clice pushed for a source file while it is shown.
/// VS Code tells the server a file closed only once nothing holds its
/// document, and it holds a document it opened for an extension for up to
/// three minutes: GitHub Pull Requests has every file opened so, to ask
/// where comments may go. The diagnostics of a file no longer shown wait
/// here for it to be shown again.
export function registerProblems(
    client: () => ClientHandle | undefined,
    selector: vscode.DocumentSelector,
    ext: vscode.ExtensionContext,
): (uri: vscode.Uri, diagnostics: vscode.Diagnostic[], next: HandleDiagnosticsSignature) => void {
    // The last push of each source file, in the collection of the client
    // that received it: a restarted client's server publishes anew.
    const pushed = new Map<
        string,
        {
            uri: vscode.Uri;
            diagnostics: vscode.Diagnostic[];
            collection: vscode.DiagnosticCollection | undefined;
        }
    >();
    let shown = shownFiles();

    const update = () => {
        const now = shownFiles();
        const collection = client()?.current.diagnostics;
        for (const [key, entry] of pushed) {
            if (entry.collection !== collection) {
                pushed.delete(key);
            } else if (now.has(key) && !shown.has(key)) {
                collection?.set(entry.uri, entry.diagnostics);
            } else if (!now.has(key) && shown.has(key)) {
                collection?.delete(entry.uri);
            }
        }
        shown = now;
    };
    ext.subscriptions.push(
        vscode.window.tabGroups.onDidChangeTabs(update),
        vscode.window.onDidChangeVisibleTextEditors(update),
    );

    return (uri, diagnostics, next) => {
        const key = uri.toString();
        pushed.delete(key);
        const document = vscode.workspace.textDocuments.find(
            (candidate) => candidate.uri.toString() === key,
        );
        // A file the server does not compile, clice.toml, keeps its problems
        // listed: nothing else tells its configuration is broken.
        if (
            document === undefined ||
            vscode.languages.match(selector, document) === 0 ||
            diagnostics.length === 0
        ) {
            next(uri, diagnostics);
            return;
        }
        pushed.set(key, { uri, diagnostics, collection: client()?.current.diagnostics });
        next(uri, shownFiles().has(key) ? diagnostics : []);
    };
}
