import * as vscode from "vscode";
import { LanguageClient } from "vscode-languageclient/node";

export type ContextItem = {
    label: string;
    description: string;
    uri: string;
    occurrence?: number;
    commandHash?: string;
};

type QueryContextResult = { contexts: ContextItem[]; total: number; epoch: number };
type CurrentContextResult = { context: ContextItem | null };
type SwitchContextResult = { success: boolean; stale?: boolean };

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
        epoch = 0,
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
            arguments: [context, epoch],
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
    epoch = 0;

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
            (context) =>
                new ContextTreeItem(context, false, sameContext(context, this.current), this.epoch),
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
        const uri = editor.document.uri.toString();
        this.uri = uri;
        this.loaded = [];
        this.total = 0;
        try {
            const [query, current] = await Promise.all([
                this.client.sendRequest<QueryContextResult>("clice/queryContext", { uri }),
                this.client.sendRequest<CurrentContextResult>("clice/currentContext", { uri }),
            ]);
            // A refresh for a previously active editor can finish after a
            // newer one started; its results belong to the wrong document.
            if (this.uri !== uri) {
                return;
            }
            this.loaded = query?.contexts ?? [];
            this.total = query?.total ?? this.loaded.length;
            this.epoch = query?.epoch ?? 0;
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
        const uri = this.uri;
        try {
            const query = await this.client.sendRequest<QueryContextResult>("clice/queryContext", {
                uri,
                offset: this.loaded.length,
            });
            if (this.uri !== uri) {
                return;
            }
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

/** Re-sync an open document with the server (didClose + didOpen) so the
 * editor re-requests every language feature — tokens, links, hints — and
 * the recompile publishes fresh diagnostics. Used after a context switch:
 * the pull-based server only re-targets the session. The language-id
 * round-trip is the only stable way to force a full re-sync; buffer
 * content and unsaved edits survive it. */
export async function resyncDocument(uri: string) {
    const doc = vscode.workspace.textDocuments.find(
        (candidate) => candidate.uri.toString() === uri,
    );
    if (!doc) {
        return;
    }
    const language = doc.languageId;
    resyncing.add(uri);
    try {
        await vscode.languages.setTextDocumentLanguage(doc, "plaintext");
        await vscode.languages.setTextDocumentLanguage(doc, language);
    } catch {
        // The document was closed mid-round-trip; nothing left to resync,
        // and the caller's UI refresh must still run.
    } finally {
        resyncing.delete(uri);
    }
}

/** Documents mid-resync: their transient plaintext hop must not be
 * mistaken by detectCxxFragment for a fragment awaiting detection — the
 * detector would race the restore and pin a c/cuda-cpp file to cpp. */
const resyncing = new Set<string>();

export function registerCompilationContext(client: LanguageClient, ext: vscode.ExtensionContext) {
    const status = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
    status.command = "clice.switchContext";
    status.tooltip = "clice: active compilation context (click to switch)";

    const tree = new ContextTreeProvider(client);

    async function refresh(editor: vscode.TextEditor | undefined) {
        void tree.refresh(editor);
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

    async function applyContext(picked: ContextItem, epoch?: number, targetUri?: string) {
        // The QuickPick flow pins the document it queried for; switching
        // editors while the pick is open must not retarget the request.
        const uri =
            targetUri ??
            tree.activeUri() ??
            vscode.window.activeTextEditor?.document.uri.toString();
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
        if (epoch !== undefined && epoch !== 0) {
            params.epoch = epoch;
        }
        const switched = await client.sendRequest<SwitchContextResult>(
            "clice/switchContext",
            params,
        );
        if (switched?.stale) {
            vscode.window.showInformationMessage(
                "clice: the workspace changed since this listing — refreshed, pick again",
            );
        } else if (!switched?.success) {
            vscode.window.showWarningMessage("clice: failed to switch compilation context");
        } else {
            // The server is pull-based: the switch only re-targets the
            // session, and the refresh is the client's job.
            await resyncDocument(uri);
            // Editors with automatic feature pulls disabled stop at the
            // reopen (didOpen alone compiles nothing); one cheap explicit
            // pull guarantees diagnostics come back regardless.
            void vscode.commands.executeCommand(
                "vscode.executeDocumentSymbolProvider",
                vscode.Uri.parse(uri),
            );
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
        let epoch = 0;

        while (true) {
            if (loaded.length < total) {
                const result = await client.sendRequest<QueryContextResult>("clice/queryContext", {
                    uri,
                    offset: loaded.length,
                });
                loaded.push(...(result?.contexts ?? []));
                total = result?.total ?? loaded.length;
                epoch = result?.epoch ?? 0;
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
                title: "Switch Compilation Context",
                placeHolder: "Compilation context to use for this file",
            });
            if (!chosen) {
                return;
            }
            if (chosen.loadMore) {
                continue;
            }
            await applyContext(chosen.context!, epoch, uri);
            return;
        }
    }

    // X-macro style fragments (.def/.inc/.inl/...) open as plain text and
    // never reach the language server. If clice knows the file is included
    // by some C++ TU (it has compilation contexts), flip its language so
    // the whole toolchain attaches.
    async function detectCxxFragment(document: vscode.TextDocument) {
        if (document.languageId !== "plaintext" || resyncing.has(document.uri.toString())) {
            return;
        }
        if (!/\.(def|inc|inl|tpp|ipp)$/.test(document.uri.fsPath)) {
            return;
        }
        try {
            const query = await client.sendRequest<QueryContextResult>("clice/queryContext", {
                uri: document.uri.toString(),
            });
            if ((query?.total ?? 0) > 0) {
                await vscode.languages.setTextDocumentLanguage(document, "cpp");
            }
        } catch {
            // Server not ready — leave the document as-is.
        }
    }

    async function showCurrent() {
        const editor = vscode.window.activeTextEditor;
        if (!isCppEditor(editor)) {
            return;
        }
        const result = await client.sendRequest<CurrentContextResult>("clice/currentContext", {
            uri: editor.document.uri.toString(),
        });
        const context = result?.context;
        if (!context) {
            vscode.window.showInformationMessage(
                "clice: automatic compilation context (no explicit selection)",
            );
            return;
        }
        const occurrence =
            context.occurrence !== undefined && context.occurrence > 0
                ? ` (occurrence #${context.occurrence + 1})`
                : "";
        vscode.window.showInformationMessage(
            `clice: ${context.label}${occurrence} — ${context.description}`,
        );
    }

    async function query() {
        await refresh(vscode.window.activeTextEditor);
        await vscode.commands.executeCommand("clice.contexts.focus");
    }

    ext.subscriptions.push(
        status,
        vscode.workspace.onDidOpenTextDocument((document) => void detectCxxFragment(document)),
        vscode.window.registerTreeDataProvider("clice.contexts", tree),
        vscode.commands.registerCommand("clice.switchContext", select),
        vscode.commands.registerCommand("clice.showCurrentContext", showCurrent),
        vscode.commands.registerCommand("clice.queryContexts", query),
        vscode.commands.registerCommand("clice.applyContext", applyContext),
        vscode.commands.registerCommand("clice.loadMoreContexts", () => tree.loadMore()),
        vscode.commands.registerCommand("clice.refreshContexts", () =>
            refresh(vscode.window.activeTextEditor),
        ),
        vscode.window.onDidChangeActiveTextEditor((editor) => void refresh(editor)),
    );
    void refresh(vscode.window.activeTextEditor);
    // Documents opened before activation never fire onDidOpenTextDocument.
    for (const document of vscode.workspace.textDocuments) {
        void detectCxxFragment(document);
    }
}
