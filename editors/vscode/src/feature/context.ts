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

function sameContext(a: ContextItem, b: ContextItem | null | undefined): boolean {
    if (!b) {
        return false;
    }
    return (
        a.uri === b.uri &&
        (a.occurrence ?? 0) === (b.occurrence ?? 0) &&
        (a.commandHash ?? "") === (b.commandHash ?? "")
    );
}

class ContextTreeItem extends vscode.TreeItem {
    constructor(
        readonly context: ContextItem | undefined,
        readonly loadMore: boolean,
        active: boolean,
    ) {
        super(
            loadMore ? "Load more…" : (context?.label ?? ""),
            vscode.TreeItemCollapsibleState.None,
        );
        if (loadMore) {
            this.iconPath = new vscode.ThemeIcon("ellipsis");
            this.command = { command: "clice.loadMoreContexts", title: "Load more" };
            return;
        }
        this.description = active ? `${context!.description} (active)` : context!.description;
        this.tooltip = context!.description;
        this.iconPath = new vscode.ThemeIcon(active ? "pass-filled" : "circle-large-outline");
        this.contextValue = "clice-context";
        this.command = {
            command: "clice.applyContext",
            title: "Switch to this context",
            arguments: [context],
        };
    }
}

class ContextTreeProvider implements vscode.TreeDataProvider<ContextTreeItem> {
    private emitter = new vscode.EventEmitter<void>();
    readonly onDidChangeTreeData = this.emitter.event;

    private loaded: ContextItem[] = [];
    private total = 0;
    private current: ContextItem | null = null;
    private uri: string | undefined;

    constructor(private client: LanguageClient) {}

    getTreeItem(element: ContextTreeItem) {
        return element;
    }

    async getChildren(element?: ContextTreeItem): Promise<ContextTreeItem[]> {
        if (element) {
            return [];
        }
        if (!this.uri) {
            return [];
        }
        const items = this.loaded.map(
            (context) => new ContextTreeItem(context, false, sameContext(context, this.current)),
        );
        if (this.loaded.length < this.total) {
            items.push(new ContextTreeItem(undefined, true, false));
        }
        return items;
    }

    async refresh(editor: vscode.TextEditor | undefined) {
        if (!isCppEditor(editor)) {
            this.uri = undefined;
            this.loaded = [];
            this.total = 0;
            this.emitter.fire();
            return;
        }
        this.uri = editor.document.uri.toString();
        this.loaded = [];
        this.total = 0;
        try {
            const [query, current] = await Promise.all([
                this.client.sendRequest<QueryContextResult>("clice/queryContext", {
                    uri: this.uri,
                }),
                this.client.sendRequest<CurrentContextResult>("clice/currentContext", {
                    uri: this.uri,
                }),
            ]);
            this.loaded = query?.contexts ?? [];
            this.total = query?.total ?? this.loaded.length;
            this.current = current?.context ?? null;
        } catch {
            // Server not ready; leave the view empty.
        }
        this.emitter.fire();
    }

    async loadMore() {
        if (!this.uri || this.loaded.length >= this.total) {
            return;
        }
        try {
            const query = await this.client.sendRequest<QueryContextResult>(
                "clice/queryContext",
                { uri: this.uri, offset: this.loaded.length },
            );
            this.loaded.push(...(query?.contexts ?? []));
            this.total = query?.total ?? this.loaded.length;
        } catch {
            // Keep what we have.
        }
        this.emitter.fire();
    }

    activeUri() {
        return this.uri;
    }
}

export function registerCompilationContext(client: LanguageClient, ext: vscode.ExtensionContext) {
    const status = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
    status.command = "clice.selectContext";
    status.tooltip = "clice: active compilation context (click to switch)";

    const tree = new ContextTreeProvider(client);

    async function refresh(editor: vscode.TextEditor | undefined) {
        void tree.refresh(editor);
        if (!isCppEditor(editor)) {
            status.hide();
            return;
        }
        try {
            const result = await client.sendRequest<CurrentContextResult>(
                "clice/currentContext",
                { uri: editor.document.uri.toString() },
            );
            const label = result?.context?.label ?? "auto";
            status.text = `$(list-tree) ${label}`;
            status.show();
        } catch {
            status.hide();
        }
    }

    async function applyContext(picked: ContextItem) {
        const uri = tree.activeUri() ?? vscode.window.activeTextEditor?.document.uri.toString();
        if (!uri) {
            return;
        }
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
        await refresh(vscode.window.activeTextEditor);
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
                const result = await client.sendRequest<QueryContextResult>(
                    "clice/queryContext",
                    { uri, offset: loaded.length },
                );
                loaded.push(...(result?.contexts ?? []));
                total = result?.total ?? loaded.length;
                if (loaded.length === 0) {
                    vscode.window.showInformationMessage(
                        "clice: no compilation contexts available for this file",
                    );
                    return;
                }
            }

            type ContextPick = vscode.QuickPickItem & {
                context?: ContextItem;
                loadMore?: boolean;
            };
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
            await applyContext(chosen.context!);
            return;
        }
    }

    ext.subscriptions.push(
        status,
        vscode.window.registerTreeDataProvider("clice.contexts", tree),
        vscode.commands.registerCommand("clice.selectContext", select),
        vscode.commands.registerCommand("clice.applyContext", applyContext),
        vscode.commands.registerCommand("clice.loadMoreContexts", () => tree.loadMore()),
        vscode.commands.registerCommand("clice.refreshContexts", () =>
            refresh(vscode.window.activeTextEditor),
        ),
        vscode.window.onDidChangeActiveTextEditor((editor) => void refresh(editor)),
    );
    void refresh(vscode.window.activeTextEditor);
}
