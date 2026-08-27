import * as vscode from "vscode";
import { State } from "vscode-languageclient/node";
import type { ClientHandle } from "../client";
// Protocol shapes come from the shared definition in tools/protocol —
// type-only, so the extension bundle carries no runtime dependency on it.
import type {
    ContextItem,
    CurrentContextResult,
    QueryContextResult,
    SwitchContextResult,
} from "@clice/tools/protocol" with { "resolution-mode": "import" };

export type { ContextItem };

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
        if (loadMore || !context) {
            this.iconPath = new vscode.ThemeIcon("ellipsis");
            this.command = { command: "clice.loadMoreContexts", title: "Load more" };
            return;
        }
        this.description = active ? `${context.description} (active)` : context.description;
        this.tooltip = context.description;
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
    /// Bumped by every refresh and loadMore; a response is dropped when a
    /// newer request started while it was in flight, including reorders of
    /// two requests for the same document.
    private generation = 0;
    epoch = 0;

    constructor(private client: ClientHandle) {}

    getTreeItem(element: ContextTreeItem) {
        return element;
    }

    getChildren(element?: ContextTreeItem): ContextTreeItem[] {
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

    /// Resolves to the fresh listing's current context, to null when there
    /// is nothing to show (non-C++ editor, request failed), and to
    /// undefined when a newer request superseded this one and owns the UI.
    async refresh(
        editor: vscode.TextEditor | undefined,
    ): Promise<{ current: ContextItem | null } | null | undefined> {
        this.generation += 1;
        const generation = this.generation;
        if (!isCppEditor(editor)) {
            this.uri = undefined;
            this.loaded = [];
            this.total = 0;
            this.emitter.fire();
            return null;
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
            if (generation !== this.generation) {
                return undefined;
            }
            this.loaded = query.contexts;
            this.total = query.total;
            this.epoch = query.epoch;
            this.current = current.context;
            this.emitter.fire();
            return { current: this.current };
        } catch {
            // Server not ready; leave the view empty.
            if (generation !== this.generation) {
                return undefined;
            }
            this.emitter.fire();
            return null;
        }
    }

    async loadMore() {
        const uri = this.uri;
        if (!uri || this.loaded.length >= this.total) {
            return;
        }
        this.generation += 1;
        const generation = this.generation;
        try {
            let query = await this.client.sendRequest<QueryContextResult>("clice/queryContext", {
                uri,
                offset: this.loaded.length,
            });
            if (generation !== this.generation) {
                return;
            }
            if (query.epoch !== this.epoch) {
                // The workspace changed under the pagination; pages of
                // different epochs must not mix, so restart the listing —
                // including the active marker, whose context may be gone.
                const [fresh, current] = await Promise.all([
                    this.client.sendRequest<QueryContextResult>("clice/queryContext", { uri }),
                    this.client.sendRequest<CurrentContextResult>("clice/currentContext", { uri }),
                ]);
                if (generation !== this.generation) {
                    return;
                }
                this.loaded = [];
                this.epoch = fresh.epoch;
                this.current = current.context;
                query = fresh;
            }
            this.loaded.push(...query.contexts);
            this.total = query.total;
            this.emitter.fire();
        } catch {
            // Keep what we have.
        }
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

export function registerCompilationContext(client: ClientHandle, ext: vscode.ExtensionContext) {
    const status = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
    status.command = "clice.switchContext";
    status.tooltip = "clice: active compilation context (click to switch)";

    const tree = new ContextTreeProvider(client);

    async function refresh(editor: vscode.TextEditor | undefined) {
        const result = await tree.refresh(editor);
        if (result === undefined) {
            // Superseded — the newer refresh owns the status bar too.
            return;
        }
        if (result === null) {
            status.hide();
            return;
        }
        // The active editor may have moved on while the request was in
        // flight; the label must not describe a different document.
        if (vscode.window.activeTextEditor !== editor) {
            return;
        }
        status.text = `$(list-tree) ${result.current?.label ?? "auto"}`;
        status.show();
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
        let switched: SwitchContextResult;
        try {
            switched = await client.sendRequest<SwitchContextResult>("clice/switchContext", params);
        } catch {
            vscode.window.showWarningMessage(
                "clice: failed to switch compilation context — is the server running?",
            );
            return;
        }
        if (switched.stale) {
            vscode.window.showInformationMessage(
                "clice: the workspace changed since this listing — refreshed, pick again",
            );
        } else if (!switched.success) {
            vscode.window.showWarningMessage("clice: failed to switch compilation context");
        } else {
            // The server is pull-based: the switch only re-targets the
            // session, and the refresh is the client's job.
            await resyncDocument(uri);
            // Editors with automatic feature pulls disabled stop at the
            // reopen (didOpen alone compiles nothing); one cheap explicit
            // pull guarantees diagnostics come back regardless.
            void vscode.commands
                .executeCommand("vscode.executeDocumentSymbolProvider", vscode.Uri.parse(uri))
                .then(undefined, () => undefined);
        }
        await refresh(vscode.window.activeTextEditor);
    }

    async function select() {
        const editor = vscode.window.activeTextEditor;
        if (!isCppEditor(editor)) {
            return;
        }
        const uri = editor.document.uri.toString();

        let loaded: ContextItem[] = [];
        let total = Number.POSITIVE_INFINITY;
        let epoch = 0;

        for (;;) {
            if (loaded.length < total) {
                let result: QueryContextResult;
                try {
                    result = await client.sendRequest<QueryContextResult>("clice/queryContext", {
                        uri,
                        offset: loaded.length,
                    });
                } catch {
                    vscode.window.showWarningMessage("clice: server not ready");
                    return;
                }
                if (loaded.length > 0 && result.epoch !== epoch) {
                    // The workspace changed between pages; restart the
                    // listing so one epoch describes every offered item.
                    loaded = [];
                    total = Number.POSITIVE_INFINITY;
                    epoch = result.epoch;
                    continue;
                }
                loaded.push(...result.contexts);
                total = result.total;
                epoch = result.epoch;
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
            if (!chosen.context) {
                continue;
            }
            await applyContext(chosen.context, epoch, uri);
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
            if (query.total > 0) {
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
        let result: CurrentContextResult;
        try {
            result = await client.sendRequest<CurrentContextResult>("clice/currentContext", {
                uri: editor.document.uri.toString(),
            });
        } catch {
            vscode.window.showInformationMessage("clice: server not ready");
            return;
        }
        const context = result.context;
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
        // Refresh the custom UI on server lifecycle edges: a (re)started
        // server re-targets every session, a stopped one has nothing to
        // show. Registration happens before the first start, so this also
        // covers the initial activation.
        client.onDidChangeState((event) => {
            if (event.newState === State.Starting) {
                return;
            }
            void refresh(vscode.window.activeTextEditor);
            if (event.newState !== State.Running) {
                return;
            }
            // Documents opened before the server was up never fire
            // onDidOpenTextDocument; sweep them once it is.
            for (const document of vscode.workspace.textDocuments) {
                void detectCxxFragment(document);
            }
        }),
    );
}
