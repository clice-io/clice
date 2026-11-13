import * as assert from "assert";
import * as fs from "fs";
import * as path from "path";
import * as vscode from "vscode";
import { State } from "vscode-languageclient/node";
import type { ClientHandle } from "../client";
// Shared protocol shapes — type-only, mirrors feature/context.ts.
import type {
    ContextItem,
    CounterpartsResult,
    CurrentContextResult,
    ListConfigurationsResult,
    QueryContextResult,
    SwitchConfigurationResult,
    SwitchContextResult,
    SyncResult,
} from "@clice/tools/protocol" with {
    "resolution-mode": "import",
};
import type { Scenario } from "@clice/tools/testing/editors/scenario" with {
    "resolution-mode": "import",
};

// End-to-end tests against a real clice, one run per fixture: the workspace
// folder is a copy of tests/editors/NN_name (.vscode-test.mjs), whose
// scenario.json says what to check. The binary comes from CLICE_EXECUTABLE.
//
// The bundled variant clears CLICE_EXECUTABLE and passes CLICE_E2E_BUNDLED_FROM
// instead: the server is staged under the extension's clice/ dir so the run
// exercises the bundled-executable fallback in extension.ts.

/// Resolves once `ready` holds: at once, called without an event, or after
/// one of `events`, called with it.
/// Hang detection: fails after a minute, naming `what` it waited for.
function until<T>(
    events: vscode.Event<T>[],
    ready: (event?: T) => boolean,
    what: string,
): Promise<void> {
    return new Promise((resolve, reject) => {
        const done = () => {
            clearTimeout(timer);
            for (const subscription of subscriptions) {
                subscription.dispose();
            }
        };
        const check = (event?: T) => {
            if (ready(event)) {
                done();
                resolve();
            }
        };
        const subscriptions = events.map((event) => event(check));
        const timer = setTimeout(() => {
            done();
            reject(new Error(`no ${what} within a minute`));
        }, 60 * 1000);
        check();
    });
}

type Writable<T> = { -readonly [K in keyof T]: T[K] };

/// What the extension shows the user beside the editor: its language status
/// items and the Compilation Contexts view, watched through the API it
/// creates them with, which this test shares with it.
class Shown implements vscode.Disposable {
    private readonly changed = new vscode.EventEmitter<void>();
    /// Fires when an item changes, appears or goes.
    readonly onDidChange = this.changed.event;
    private readonly items = new Map<string, vscode.LanguageStatusItem>();
    private readonly restore: (() => void)[] = [];
    contexts: vscode.TreeDataProvider<vscode.TreeItem> | undefined;

    constructor() {
        const languages = vscode.languages as Writable<typeof vscode.languages>;
        const create = languages.createLanguageStatusItem;
        languages.createLanguageStatusItem = (id, selector) => {
            const item = create(id, selector);
            const fire = () => {
                this.changed.fire();
            };
            const watched = new Proxy(item, {
                get: (target, key) => {
                    if (key === "dispose") {
                        return () => {
                            if (this.items.get(id) === watched) {
                                this.items.delete(id);
                            }
                            target.dispose();
                            fire();
                        };
                    }
                    const value: unknown = Reflect.get(target, key);
                    return typeof value === "function"
                        ? (value as (this: unknown, ...args: unknown[]) => unknown).bind(target)
                        : value;
                },
                set: (target, key, value) => {
                    Reflect.set(target, key, value);
                    fire();
                    return true;
                },
            });
            this.items.set(id, watched);
            fire();
            return watched;
        };
        const window = vscode.window as Writable<typeof vscode.window>;
        const register = window.registerTreeDataProvider;
        window.registerTreeDataProvider = <T>(id: string, provider: vscode.TreeDataProvider<T>) => {
            if (id === "clice.contexts") {
                this.contexts = provider as vscode.TreeDataProvider<vscode.TreeItem>;
            }
            return register(id, provider);
        };
        this.restore.push(() => {
            languages.createLanguageStatusItem = create;
            window.registerTreeDataProvider = register;
        });
    }

    /// The language status item `id` once `ready` holds of it (undefined
    /// while it is hidden).
    async status(
        id: string,
        ready: (item: vscode.LanguageStatusItem | undefined) => boolean,
        what: string,
    ): Promise<vscode.LanguageStatusItem | undefined> {
        await until([this.onDidChange], () => ready(this.items.get(id)), what);
        return this.items.get(id);
    }

    dispose() {
        for (const restore of this.restore) {
            restore();
        }
        this.changed.dispose();
    }
}

/// Has each quick pick `run` shows choose the item `choose` picks of those
/// offered, failing when it picks none.
async function choosing<T>(
    choose: (offered: readonly vscode.QuickPickItem[]) => vscode.QuickPickItem | undefined,
    run: () => Thenable<T>,
): Promise<T> {
    const window = vscode.window as Writable<typeof vscode.window>;
    const show = window.showQuickPick;
    window.showQuickPick = (async (
        items: readonly vscode.QuickPickItem[] | Thenable<readonly vscode.QuickPickItem[]>,
    ) => {
        const offered = await items;
        const chosen = choose(offered);
        assert.ok(
            chosen,
            `nothing to choose among: ${offered.map((item) => item.label).join(", ")}`,
        );
        return chosen;
    }) as unknown as typeof window.showQuickPick;
    try {
        return await run();
    } finally {
        window.showQuickPick = show;
    }
}

suite("clice E2E", function () {
    // The bundled variant runs the server staged under clice/ by .vscode-test.mjs;
    // its CLICE_EXECUTABLE is empty and CLICE_E2E_BUNDLED_FROM carries the origin.
    const bundled = !!process.env.CLICE_E2E_BUNDLED_FROM;
    const executable = bundled ? process.env.CLICE_E2E_BUNDLED_FROM : process.env.CLICE_EXECUTABLE;

    let root: string;
    let scenario: Scenario;
    let shown: Shown;
    let document: vscode.TextDocument;
    let position: vscode.Position;

    function extension(): vscode.Extension<{ client: ClientHandle }> {
        const found = vscode.extensions.getExtension<{ client: ClientHandle }>("clice-io.clice");
        assert.ok(found?.isActive, "extension not active");
        return found;
    }

    function client(): ClientHandle {
        return extension().exports.client;
    }

    function opened(): vscode.TextDocument {
        assert.ok(document, "the scenario's file was not opened (earlier test failed)");
        return document;
    }

    function uriOf(file: string): vscode.Uri {
        return vscode.Uri.file(path.join(root, file));
    }

    /// A file of the fixture, as the scenario names it.
    function relative(uri: string | vscode.Uri): string {
        const file = typeof uri === "string" ? vscode.Uri.parse(uri) : uri;
        return path.relative(root, file.fsPath).split(path.sep).join("/");
    }

    async function sync() {
        const { pending } = await client().sendRequest<SyncResult>("clice/internal/sync", {});
        assert.deepStrictEqual(pending, [], "the server is still busy");
    }

    async function errors(uri: vscode.Uri): Promise<string[]> {
        await sync();
        return vscode.languages
            .getDiagnostics(uri)
            .filter((diagnostic) => diagnostic.severity === vscode.DiagnosticSeverity.Error)
            .map((diagnostic) => `${diagnostic.range.start.line + 1}: ${diagnostic.message}`);
    }

    /// Resolves once the server has stopped and run again.
    function restarted(what: string): Promise<void> {
        let stopped = false;
        return until(
            [client().onDidChangeState],
            (event) => {
                stopped ||= event?.newState === State.Stopped;
                return stopped && event?.newState === State.Running;
            },
            what,
        );
    }

    suiteSetup(async function () {
        this.timeout(60 * 1000);
        assert.ok(executable, "CLICE_EXECUTABLE names the server under test");
        const folder = vscode.workspace.workspaceFolders?.[0];
        assert.ok(folder, "no workspace folder");
        root = folder.uri.fsPath;
        scenario = JSON.parse(
            fs.readFileSync(path.join(root, "scenario.json"), "utf8"),
        ) as Scenario;

        assert.ok(
            !vscode.extensions.getExtension("clice-io.clice")?.isActive,
            "the extension started before the test could watch what it shows",
        );
        shown = new Shown();

        // The bundled fallback is only taken when clice.executable is unset,
        // so leave it alone for that variant (its user-data-dir is isolated).
        if (!bundled) {
            await vscode.workspace
                .getConfiguration("clice")
                .update("executable", path.resolve(executable), vscode.ConfigurationTarget.Global);
        }
    });

    suiteTeardown(function () {
        shown.dispose();
    });

    test("server starts and publishes diagnostics", async function () {
        this.timeout(240 * 1000);
        const uri = uriOf(scenario.file);
        const diagnostics = until(
            [vscode.languages.onDidChangeDiagnostics],
            (event) => !!event?.uris.some((each) => each.toString() === uri.toString()),
            `diagnostics of ${scenario.file}`,
        );

        // Opening the C++ document activates the extension, which starts clice.
        document = await vscode.workspace.openTextDocument(uri);
        await vscode.window.showTextDocument(document);

        const offset = document.getText().indexOf(scenario.symbol);
        assert.ok(offset >= 0, `symbol not found: ${scenario.symbol}`);
        position = document.positionAt(offset);

        await diagnostics;
        // clice offers pulls to a client declaring them, as the extension does.
        assert.notStrictEqual(
            client().current.initializeResult?.capabilities.diagnosticProvider,
            undefined,
            "diagnostics are pulled",
        );
    });

    test("workspace symbol indexed, the file clean", async function () {
        // Definition is index-based. The fixture turns the test hooks on
        // (tools/testing/editors/fixtures.ts): the server answers a sync once
        // the background index is done.
        assert.deepStrictEqual(await errors(opened().uri), [], `${scenario.file} has errors`);
        const symbols = await vscode.commands.executeCommand<vscode.SymbolInformation[]>(
            "vscode.executeWorkspaceSymbolProvider",
            scenario.indexed.split("(")[0],
        );
        assert.ok(
            symbols.some((s) => s.name === scenario.indexed),
            `symbol ${scenario.indexed} not indexed`,
        );
    });

    test("hover", async function () {
        this.timeout(60 * 1000);
        const hovers = await vscode.commands.executeCommand<vscode.Hover[]>(
            "vscode.executeHoverProvider",
            opened().uri,
            position,
        );
        assert.ok(hovers.length > 0, "hover returned no results");
        assert.ok(hovers[0].contents.length > 0, "hover returned empty contents");
    });

    test("semantic tokens through middleware", async function () {
        this.timeout(60 * 1000);
        // The provider path runs through the inactive-regions middleware,
        // so a decode failure or a swallowed response surfaces here as
        // missing tokens.
        const tokens = await vscode.commands.executeCommand<vscode.SemanticTokens | undefined>(
            "vscode.provideDocumentSemanticTokens",
            opened().uri,
        );
        assert.ok(tokens && tokens.data.length > 0, "no semantic tokens through the middleware");
    });

    test("definition", async function () {
        this.timeout(60 * 1000);
        const definition = scenario.definition;
        if (definition === undefined) {
            this.skip();
        }
        const locations = await vscode.commands.executeCommand<
            (vscode.Location | vscode.LocationLink)[]
        >("vscode.executeDefinitionProvider", opened().uri, position);
        assert.ok(locations.length > 0, "definition returned no locations");

        const first = locations[0];
        assert.strictEqual(
            relative(first instanceof vscode.Location ? first.uri : first.targetUri),
            definition,
        );
    });

    test("compilation contexts", async function () {
        this.timeout(180 * 1000);
        const contexts = scenario.contexts;
        if (contexts === undefined) {
            this.skip();
        }
        const uri = opened().uri.toString();
        const current = () =>
            client().sendRequest<CurrentContextResult>("clice/currentContext", { uri });
        const status = (text: string, detail: (detail: string) => boolean = () => true) =>
            shown.status(
                "clice.context",
                (item) => item?.text === text && detail(item.detail ?? ""),
                `context status "${text}"`,
            );

        const automatic = await current();
        assert.ok(automatic.automatic, "no choice is in force at first");
        assert.strictEqual(relative(automatic.context?.uri ?? ""), contexts.automatic);
        await status("auto");
        const query = await client().sendRequest<QueryContextResult>("clice/queryContext", { uri });
        assert.strictEqual(query.total, contexts.total);

        // The client contract: a switch through the extension's commands
        // keeps the document open. The server answers once the choice is
        // in force; the client pulls the diagnostics anew only when they
        // changed, and the header compiles clean under either host.
        const switchContext = (
            choose: (offered: readonly vscode.QuickPickItem[]) => vscode.QuickPickItem | undefined,
        ) =>
            choosing(choose, async () => {
                await vscode.window.showTextDocument(opened());
                await vscode.commands.executeCommand("clice.switchContext");
            });
        const labelled =
            (label: (label: string) => boolean) => (offered: readonly vscode.QuickPickItem[]) =>
                offered.find((item) => label(item.label));
        // The pick offers a page of contexts at a time.
        const other = path.posix.basename(contexts.other);
        await switchContext(
            (offered) =>
                labelled((label) => label === other)(offered) ??
                labelled((label) => label.includes("Load more"))(offered),
        );
        const switched = await current();
        assert.ok(!switched.automatic, "the switched host is the user's choice");
        assert.strictEqual(relative(switched.context?.uri ?? ""), contexts.other);
        assert.strictEqual(document.languageId, "cpp", "the switch keeps the document as it was");
        await status("selected");

        // Beside its own context, the file shows the code its other contexts
        // compile until the user hides it.
        assert.ok(switched.otherContexts, "other contexts show by default");
        await switchContext(labelled((label) => label.endsWith("Hide other contexts")));
        assert.ok(!(await current()).otherContexts, "other contexts hidden");
        await status("selected", (detail) => detail.endsWith(", other contexts hidden"));
        await switchContext(labelled((label) => label.endsWith("Show other contexts")));
        assert.ok((await current()).otherContexts, "other contexts shown again");
        await status("selected", (detail) => !detail.endsWith(", other contexts hidden"));

        await vscode.window.showTextDocument(opened());
        await vscode.commands.executeCommand("clice.resetContext");
        assert.ok((await current()).automatic, "the reset leaves the automatic context");
        await status("auto");
    });

    test("the Compilation Contexts view", async function () {
        this.timeout(180 * 1000);
        const contexts = scenario.contexts;
        if (contexts === undefined) {
            this.skip();
        }
        const uri = opened().uri.toString();
        const view = shown.contexts;
        assert.ok(view, "the extension registered no Compilation Contexts view");
        const changed = view.onDidChangeTreeData as vscode.Event<unknown> | undefined;
        assert.ok(changed, "the view announces no change");
        const context = (item: vscode.TreeItem) =>
            item.command?.command === "clice.applyContext"
                ? (item.command.arguments?.[0] as ContextItem)
                : undefined;
        /// What the view lists: the contexts, those marked in use, whether
        /// more can load.
        const listed = () => {
            const items = view.getChildren();
            assert.ok(Array.isArray(items), "the view lists its items at once");
            return {
                contexts: items.flatMap((item) => {
                    const each = context(item);
                    return each === undefined ? [] : [relative(each.uri)];
                }),
                active: items
                    .filter(
                        (item) =>
                            (item.iconPath as vscode.ThemeIcon | undefined)?.id === "pass-filled",
                    )
                    .map((item) => relative(context(item)?.uri ?? "")),
                more: items.some((item) => item.command?.command === "clice.loadMoreContexts"),
            };
        };
        const showing = (ready: (now: ReturnType<typeof listed>) => boolean, what: string) =>
            until([changed], () => ready(listed()), what);

        // A choice made past the extension, as another client's source action
        // makes it, shows once the view is refreshed.
        await vscode.window.showTextDocument(opened());
        const query = await client().sendRequest<QueryContextResult>("clice/queryContext", { uri });
        const target = query.contexts.find((each) => relative(each.uri) !== contexts.automatic);
        assert.ok(target, "the first page lists another context than the automatic one");
        const result = await client().sendRequest<SwitchContextResult>("clice/switchContext", {
            uri,
            contextUri: target.uri,
            epoch: query.epoch,
        });
        assert.ok(result.success, "the switch past the extension succeeded");
        await vscode.commands.executeCommand("clice.refreshContexts");
        await showing(
            (now) => now.active.length === 1 && now.active[0] === relative(target.uri),
            `the view marking ${relative(target.uri)}`,
        );
        await client().sendRequest<SwitchContextResult>("clice/resetContext", { uri });

        const page = Math.min(contexts.total, 10);
        await vscode.commands.executeCommand("clice.refreshContexts");
        await showing(
            (now) => now.contexts.length === page && now.more === contexts.total > page,
            "the first page of the contexts",
        );
        assert.ok(
            listed().contexts.includes(contexts.automatic),
            "the view lists the automatic context",
        );

        if (contexts.total > page) {
            await vscode.commands.executeCommand("clice.loadMoreContexts");
            await showing(
                (now) => now.contexts.length === contexts.total && !now.more,
                "every context loaded",
            );
            assert.ok(
                listed().contexts.includes(contexts.other),
                `the view lists ${contexts.other}`,
            );
        }
    });

    test("switch source/header", async function () {
        this.timeout(120 * 1000);
        const counterpart = scenario.counterpart;
        if (counterpart === undefined) {
            this.skip();
        }
        const manifest = extension().packageJSON as {
            contributes: { menus: { "editor/context": { command: string; group?: string }[] } };
        };
        const menu = manifest.contributes.menus["editor/context"].find(
            ({ command }) => command === "clice.switchSourceHeader",
        );
        assert.ok(menu?.group?.startsWith("navigation@"), "the editor menu offers the command");
        const active = () => relative(vscode.window.activeTextEditor?.document.uri ?? "");
        await vscode.window.showTextDocument(opened());

        if (counterpart === null) {
            // The command says there is nothing to switch to and leaves the
            // editor where it was.
            const result = await client().sendRequest<CounterpartsResult>("clice/counterparts", {
                uri: opened().uri.toString(),
            });
            assert.deepStrictEqual(result, { candidates: [], preferred: null });
            await vscode.commands.executeCommand("clice.switchSourceHeader");
            assert.strictEqual(active(), scenario.file);
            return;
        }
        await vscode.commands.executeCommand("clice.switchSourceHeader");
        await until(
            [vscode.window.onDidChangeActiveTextEditor],
            () => active() === counterpart,
            `${counterpart} in the editor`,
        );
        await vscode.commands.executeCommand("clice.switchSourceHeader");
        await until(
            [vscode.window.onDidChangeActiveTextEditor],
            () => active() === scenario.file,
            `${scenario.file} back in the editor`,
        );
    });

    test("the compile command of the file", async function () {
        this.timeout(60 * 1000);
        const current = await client().sendRequest<CurrentContextResult>("clice/currentContext", {
            uri: opened().uri.toString(),
        });
        const command = scenario.command;
        assert.ok(current.command, `${scenario.file} has no command`);
        assert.strictEqual(current.command.source, command.source);
        if (command.from !== undefined) {
            assert.strictEqual(relative(current.command.from ?? ""), command.from);
        }
        await vscode.window.showTextDocument(opened());
        await shown.status(
            "clice.compileCommand",
            (item) => item?.text === command.source,
            `compile command status "${command.source}"`,
        );

        await vscode.commands.executeCommand("clice.showCompileCommand");
        const shownCommand = vscode.window.activeTextEditor?.document;
        assert.strictEqual(shownCommand?.uri.scheme, "clice-command");
        const text = shownCommand.getText();
        assert.ok(
            text.startsWith(
                `# The command clice compiles ${path.posix.basename(scenario.file)} with:`,
            ),
            text,
        );
        // As the database spells it, which on Windows may write the drive
        // and the separators otherwise than VS Code does.
        const spelled = (file: string) =>
            process.platform === "win32" ? file.replaceAll("\\", "/").toLowerCase() : file;
        assert.ok(
            spelled(text).includes(spelled(opened().uri.fsPath)),
            "the command names the file",
        );
        await vscode.commands.executeCommand("workbench.action.closeActiveEditor");
    });

    test("the commands the extension offers", async function () {
        const commands = new Set(await vscode.commands.getCommands(true));
        assert.ok(commands.has("clice.showCompileCommand"));
        assert.ok(commands.has("clice.switchContext"));
        // The language status and the Compilation Contexts view show what
        // these did.
        assert.ok(!commands.has("clice.showCurrentContext"));
        assert.ok(!commands.has("clice.queryContexts"));
    });

    test("source actions switching contexts stay out of VS Code", async function () {
        this.timeout(60 * 1000);
        if (scenario.contexts === undefined) {
            this.skip();
        }
        // The server offers them, for editors without the extension's UI.
        const start = { line: 0, character: 0 };
        const offered = await client().sendRequest<{ kind?: string }[]>("textDocument/codeAction", {
            textDocument: { uri: opened().uri.toString() },
            range: { start, end: start },
            context: { diagnostics: [], only: ["source"], triggerKind: 1 },
        });
        assert.ok(offered.some((action) => action.kind === "source.clice.context"));

        const listed = await vscode.commands.executeCommand<vscode.CodeAction[]>(
            "vscode.executeCodeActionProvider",
            opened().uri,
            new vscode.Range(0, 0, 0, 0),
            vscode.CodeActionKind.Source.value,
        );
        assert.ok(
            !listed.some((action) => action.kind?.value.startsWith("source.clice.")),
            "VS Code lists no clice source action",
        );
    });

    test("completion", async function () {
        this.timeout(60 * 1000);
        const completions = await vscode.commands.executeCommand<vscode.CompletionList>(
            "vscode.executeCompletionItemProvider",
            opened().uri,
            position.translate(0, 1),
        );
        assert.ok(completions.items.length > 0, "completion returned no items");
    });

    test("space trigger gated outside imports", async function () {
        this.timeout(60 * 1000);
        // The middleware swallows space-triggered requests on non-import
        // lines; the cursor sits inside ordinary code here. VS Code still
        // contributes word-based suggestions (kind Text), so assert that
        // nothing beyond those — i.e. no server-provided item — shows up.
        const completions = await vscode.commands.executeCommand<vscode.CompletionList>(
            "vscode.executeCompletionItemProvider",
            opened().uri,
            position.translate(0, 1),
            " ",
        );
        const serverItems = completions.items.filter(
            (item) => item.kind !== undefined && item.kind !== vscode.CompletionItemKind.Text,
        );
        const labels = serverItems.slice(0, 10).map((item) => item.label);
        assert.strictEqual(
            serverItems.length,
            0,
            `space trigger outside an import line must yield no server items, got: ${JSON.stringify(labels)}`,
        );
    });

    test("refactor command applies its action", async function () {
        this.timeout(90 * 1000);
        const refactor = scenario.refactor;
        if (refactor === undefined) {
            this.skip();
        }

        // Each command asks for the kind its name spells: the commands and
        // the refactoring kinds the server advertises are the same set.
        const manifest = extension().packageJSON as {
            contributes: { commands: { command: string }[] };
        };
        const commands = manifest.contributes.commands
            .map(({ command }) => command)
            .filter((command) => command.startsWith("clice.refactor."))
            .map((command) => command.slice("clice.".length));
        const provider = client().current.initializeResult?.capabilities.codeActionProvider;
        const advertised =
            typeof provider === "object"
                ? (provider.codeActionKinds ?? []).filter((kind) => kind.startsWith("refactor."))
                : [];
        assert.deepStrictEqual([...commands].sort(), [...advertised].sort());

        const source = await vscode.workspace.openTextDocument(uriOf(refactor.file));
        try {
            const cursor = source.positionAt(source.getText().indexOf(refactor.at));
            await vscode.window.showTextDocument(source, {
                selection: new vscode.Range(cursor, cursor),
            });
            const edited = until(
                [vscode.workspace.onDidChangeTextDocument],
                (event) => event?.document === source && event.contentChanges.length > 0,
                "edit from the command",
            );
            await Promise.all([edited, vscode.commands.executeCommand(`clice.${refactor.kind}`)]);
            assert.ok(
                source.getText().includes(refactor.adds),
                `the edit did not add ${refactor.adds}:\n${source.getText()}`,
            );
        } finally {
            await vscode.window.showTextDocument(source);
            await vscode.commands.executeCommand("workbench.action.revertAndCloseActiveEditor");
        }
    });

    test("switch build configuration", async function () {
        this.timeout(180 * 1000);
        const configurations = scenario.configurations;
        if (configurations === undefined) {
            this.skip();
        }
        const uri = opened().uri.toString();
        const active = async () =>
            (
                await client().sendRequest<ListConfigurationsResult>("clice/listConfigurations", {
                    uri,
                })
            ).active;
        const status = (name: string) =>
            shown.status(
                "clice.configuration",
                (item) => item?.text === name,
                `configuration status "${name}"`,
            );
        await vscode.window.showTextDocument(opened());
        assert.strictEqual(await active(), configurations.active);
        await status(configurations.active);

        // The server takes the choice at its next start, which it asks for.
        const switched = restarted(`restart under ${configurations.switch}`);
        await choosing(
            (offered) => offered.find((item) => item.label === configurations.switch),
            () => vscode.commands.executeCommand("clice.switchConfiguration"),
        );
        await switched;
        assert.strictEqual(await active(), configurations.switch);
        await status(configurations.switch);
        assert.deepStrictEqual(await errors(opened().uri), [], `${scenario.file} has errors`);

        const restored = restarted(`restart under ${configurations.active}`);
        const back = await client().sendRequest<SwitchConfigurationResult>(
            "clice/switchConfiguration",
            { name: configurations.active, uri },
        );
        assert.ok(back.success, back.reason ?? "");
        await restored;
        assert.strictEqual(await active(), configurations.active);
    });

    test("the restart command restarts the server", async function () {
        this.timeout(120 * 1000);
        const before = client().current;
        const again = restarted("restart by the command");
        await vscode.commands.executeCommand("clice.restart");
        await again;
        assert.notStrictEqual(client().current, before, "a new client runs the new server");
        await sync();
        const hovers = await vscode.commands.executeCommand<vscode.Hover[]>(
            "vscode.executeHoverProvider",
            opened().uri,
            position,
        );
        assert.ok(hovers.length > 0, "the new server answers hover");
    });

    test("an edited configuration restarts the server", async function () {
        this.timeout(120 * 1000);
        const config = path.join(root, "clice.toml");
        const original = fs.existsSync(config) ? fs.readFileSync(config, "utf8") : undefined;
        // The server looks at its configuration files on every tick, polling
        // off or not, and asks the extension, which restarts it.
        const edited = restarted("restart after the edit");
        fs.writeFileSync(config, `${original ?? ""}\n# edited\n`);
        await edited;
        // The new server reads its configuration once the client says it is
        // initialized: a restore before then is simply what it reads.
        await client().sendRequest<SyncResult>("clice/internal/sync", {});
        const restored = restarted("restart after the restore");
        if (original === undefined) {
            fs.rmSync(config);
        } else {
            fs.writeFileSync(config, original);
        }
        await restored;
    });
});
