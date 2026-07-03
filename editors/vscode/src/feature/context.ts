import * as vscode from "vscode";
import { LanguageClient } from "vscode-languageclient/node";

export type ContextItem = {
    label: string;
    description: string;
    uri: string;
    occurrence?: number;
    commandHash?: string;
};

type QueryContextResult = { contexts: ContextItem[]; total: number };
type CurrentContextResult = { context: ContextItem | null };
type SwitchContextResult = { success: boolean };

function isCppEditor(editor: vscode.TextEditor | undefined): editor is vscode.TextEditor {
    const language = editor?.document.languageId;
    return language === "c" || language === "cpp" || language === "cuda-cpp";
}

type ContextPick = vscode.QuickPickItem & {
    context?: ContextItem;
    loadMore?: boolean;
};

export function registerCompilationContext(client: LanguageClient, ext: vscode.ExtensionContext) {
    const status = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
    status.command = "clice.selectContext";
    status.tooltip = "clice: active compilation context (click to switch)";

    async function refresh(editor: vscode.TextEditor | undefined) {
        if (!isCppEditor(editor)) {
            status.hide();
            return;
        }
        try {
            const result = await client.sendRequest<CurrentContextResult>("clice/currentContext", {
                uri: editor.document.uri.toString(),
            });
            const label = result?.context?.label ?? "auto";
            status.text = `$(list-tree) ${label}`;
            status.show();
        } catch {
            status.hide();
        }
    }

    async function select() {
        const editor = vscode.window.activeTextEditor;
        if (!isCppEditor(editor)) {
            return;
        }
        const uri = editor.document.uri.toString();

        const loaded: ContextItem[] = [];
        let total = Number.POSITIVE_INFINITY;

        while (true) {
            if (loaded.length < total) {
                const result = await client.sendRequest<QueryContextResult>("clice/queryContext", {
                    uri,
                    offset: loaded.length,
                });
                loaded.push(...(result?.contexts ?? []));
                total = result?.total ?? loaded.length;
                if (loaded.length === 0) {
                    vscode.window.showInformationMessage(
                        "clice: no compilation contexts available for this file",
                    );
                    return;
                }
            }

            const items: ContextPick[] = loaded.map((context) => ({
                label: context.label,
                description: context.description,
                context,
            }));
            if (loaded.length < total) {
                items.push({
                    label: `$(ellipsis) Load more (${loaded.length}/${total})`,
                    loadMore: true,
                });
            }

            const chosen = await vscode.window.showQuickPick(items, {
                title: "Select Compilation Context",
                placeHolder: "Compilation context to use for this file",
            });
            if (!chosen) {
                return;
            }
            if (chosen.loadMore) {
                continue;
            }

            const picked = chosen.context!;
            const params: Record<string, unknown> = { uri, contextUri: picked.uri };
            if (picked.occurrence !== undefined) {
                params.occurrence = picked.occurrence;
            }
            if (picked.commandHash !== undefined) {
                params.commandHash = picked.commandHash;
            }
            const switched = await client.sendRequest<SwitchContextResult>(
                "clice/switchContext",
                params,
            );
            if (!switched?.success) {
                vscode.window.showWarningMessage("clice: failed to switch compilation context");
            }
            await refresh(editor);
            return;
        }
    }

    ext.subscriptions.push(
        status,
        vscode.commands.registerCommand("clice.selectContext", select),
        vscode.window.onDidChangeActiveTextEditor((editor) => void refresh(editor)),
    );
    void refresh(vscode.window.activeTextEditor);
}
