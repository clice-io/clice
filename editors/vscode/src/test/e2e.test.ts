import * as assert from "assert";
import * as cp from "child_process";
import * as fs from "fs";
import * as os from "os";
import * as path from "path";
import * as vscode from "vscode";
import type { ClientHandle } from "../client";
// Shared protocol shapes — type-only, mirrors feature/context.ts.
import type {
    CounterpartsResult,
    CurrentContextResult,
    QueryContextResult,
} from "@clice/tools/protocol" with {
    "resolution-mode": "import",
};

import { inactiveRuns } from "../feature/inactive";
import { exited, retire } from "../process";
import { resolveExecutable } from "../setting";

// E2E smoke tests against a real clice binary. The binary path comes from
// CLICE_EXECUTABLE; without it (plain `npm test`) the suite is skipped.
// The workspace folder is set by .vscode-test.mjs and selects the scenario.
//
// The bundled variant clears CLICE_EXECUTABLE and passes CLICE_E2E_BUNDLED_FROM
// instead: the server is staged under the extension's clice/ dir so the run
// exercises the bundled-executable fallback in extension.ts.

// Keep in sync with editors/nvim/tests/e2e.lua.
interface Scenario {
    file: string;
    symbol: string;
    indexSymbol: string;
    // Absent when cross-file definition is not expected to work for the
    // scenario (definition into headers is a known index gap).
    definitionFile?: string;
}

const scenarios: Record<string, Scenario> = {
    hello_world: {
        file: "main.cpp",
        symbol: "add(1, 2)",
        indexSymbol: "add",
        definitionFile: "main.cpp",
    },
    hover_on_imported_symbol: {
        file: "use.cpp",
        symbol: "magic_number()",
        indexSymbol: "magic_number",
        definitionFile: "defs.cppm",
    },
    header_context: {
        file: "utils.h",
        symbol: "distance(p",
        indexSymbol: "calc",
    },
};

// Pure decoding of the inactive-modifier line runs; no server involved.
// Entries are (deltaLine, deltaStart, length, type, modifiers) tuples.
suite("inactive run decoding", function () {
    const MASK = 1 << 3;

    test("merges runs and bridges token-free gaps", function () {
        const data = [
            ...[1, 0, 3, 0, MASK], // line 1, inactive
            ...[2, 0, 3, 0, MASK], // line 3: the blank line between bridges
            ...[1, 0, 3, 0, 0], // line 4, active — ends the run
            ...[2, 0, 3, 0, MASK], // line 6, a separate region
        ];
        assert.deepStrictEqual(inactiveRuns(data, MASK), [
            [1, 3],
            [6, 6],
        ]);
    });

    test("several tokens on one line", function () {
        const data = [...[0, 0, 2, 0, MASK], ...[0, 3, 2, 0, MASK]];
        assert.deepStrictEqual(inactiveRuns(data, MASK), [[0, 0]]);
    });

    test("empty without the modifier", function () {
        assert.deepStrictEqual(inactiveRuns([1, 0, 3, 0, 0], MASK), []);
        assert.deepStrictEqual(inactiveRuns([], MASK), []);
        assert.deepStrictEqual(inactiveRuns([1, 0, 3, 0, MASK], 0), []);
    });
});

suite("executable setting", function () {
    test("a relative path resolves against the workspace", function () {
        const base = path.resolve("ws", "app");
        assert.strictEqual(
            resolveExecutable("../bin/clice", base),
            path.resolve(base, "../bin/clice"),
        );
        assert.strictEqual(resolveExecutable("bin/clice", undefined), "bin/clice");
    });

    test("a command name and an absolute path stay", function () {
        const base = path.resolve("ws", "app");
        const absolute = path.resolve("opt", "clice");
        assert.strictEqual(resolveExecutable("clice", base), "clice");
        assert.strictEqual(resolveExecutable(absolute, base), absolute);
    });
});

// Symlinks need a privilege Windows runners lack.
if (process.platform !== "win32") {
    suite("second names", function () {
        test("an editor on a second name moves to the open document", async function () {
            this.timeout(30 * 1000);
            const root = fs.mkdtempSync(path.join(os.tmpdir(), "clice-alias-"));
            fs.mkdirSync(path.join(root, "real"));
            fs.writeFileSync(path.join(root, "real", "a.cpp"), "int a() { return 0; }\n");
            fs.symlinkSync(path.join(root, "real"), path.join(root, "link"));
            try {
                const first = await vscode.workspace.openTextDocument(
                    vscode.Uri.file(path.join(root, "real", "a.cpp")),
                );
                await vscode.window.showTextDocument(first);
                const second = await vscode.workspace.openTextDocument(
                    vscode.Uri.file(path.join(root, "link", "a.cpp")),
                );
                await vscode.window.showTextDocument(second, {
                    selection: new vscode.Range(0, 4, 0, 5),
                    preview: false,
                });
                const deadline = Date.now() + 10 * 1000;
                const tabs = () =>
                    vscode.window.tabGroups.all
                        .flatMap((group) => group.tabs)
                        .filter((tab) => tab.input instanceof vscode.TabInputText)
                        .map((tab) => (tab.input as vscode.TabInputText).uri.toString());
                const settled = () =>
                    !tabs().includes(second.uri.toString()) &&
                    vscode.window.activeTextEditor?.document === first;
                while (!settled() && Date.now() < deadline) {
                    await new Promise((resolve) => setTimeout(resolve, 100));
                }
                assert.ok(!tabs().includes(second.uri.toString()), "the second name's tab closes");
                const active = vscode.window.activeTextEditor;
                assert.strictEqual(active?.document.uri.toString(), first.uri.toString());
                assert.deepStrictEqual(active.selection.start, new vscode.Position(0, 4));
            } finally {
                await vscode.commands.executeCommand("workbench.action.closeAllEditors");
                fs.rmSync(root, { recursive: true, force: true });
            }
        });
    });
}

// A previous server must be gone before the next one starts: it holds the
// workspace's index lock until it has saved.
suite("server retirement", function () {
    // The extension host's executable runs scripts as plain Node.
    const stub = (script: string) =>
        cp.spawn(process.execPath, ["-e", script], {
            env: { ...process.env, ELECTRON_RUN_AS_NODE: "1" },
        });

    test("waits for a server that exits", async function () {
        const server = stub("setTimeout(() => process.exit(0), 300)");
        const lines: string[] = [];
        await retire(server, { exit: 10_000, term: 10_000 }, (line) => {
            lines.push(line);
        });
        assert.ok(exited(server));
        assert.deepStrictEqual(lines, []);
    });

    test("kills a server that does not exit", async function () {
        const server = stub("process.on('SIGTERM', () => {}); setInterval(() => {}, 1000)");
        const lines: string[] = [];
        await retire(server, { exit: 2_000, term: 500 }, (line) => {
            lines.push(line);
        });
        assert.ok(exited(server));
        assert.strictEqual(lines.length, process.platform === "win32" ? 1 : 2);
    });
});

suite("clice E2E", function () {
    // The bundled variant runs the server staged under clice/ by .vscode-test.mjs;
    // its CLICE_EXECUTABLE is empty and CLICE_E2E_BUNDLED_FROM carries the origin.
    const bundled = !!process.env.CLICE_E2E_BUNDLED_FROM;
    const executable = bundled ? process.env.CLICE_E2E_BUNDLED_FROM : process.env.CLICE_EXECUTABLE;
    if (!executable) {
        if (process.env.CI) {
            throw new Error("CLICE_EXECUTABLE must be set in CI");
        }
        return;
    }

    let document: vscode.TextDocument;
    let position: vscode.Position;
    let scenario: Scenario;

    function workspaceFolder(): vscode.WorkspaceFolder {
        const folder = vscode.workspace.workspaceFolders?.[0];
        assert.ok(folder, "no workspace folder");
        return folder;
    }

    suiteSetup(async function () {
        this.timeout(60 * 1000);

        const folder = workspaceFolder();
        scenario = scenarios[path.basename(folder.uri.fsPath)];
        assert.ok(scenario, `no scenario for workspace ${folder.uri.fsPath}`);

        // The bundled fallback is only taken when clice.executable is unset,
        // so leave it alone for that variant (its user-data-dir is isolated).
        if (!bundled) {
            await vscode.workspace
                .getConfiguration("clice")
                .update("executable", path.resolve(executable), vscode.ConfigurationTarget.Global);
        }
    });

    suiteTeardown(function () {
        if (bundled) {
            const extension = vscode.extensions.getExtension("clice-io.clice");
            if (extension) {
                fs.rmSync(path.join(extension.extensionPath, "clice"), {
                    recursive: true,
                    force: true,
                });
            }
        }
    });

    test("server starts and publishes diagnostics", async function () {
        this.timeout(240 * 1000);

        const folder = workspaceFolder();
        const uri = vscode.Uri.joinPath(folder.uri, scenario.file);

        const diagnostics = new Promise<void>((resolve) => {
            const listener = vscode.languages.onDidChangeDiagnostics((event) => {
                if (event.uris.some((u) => u.toString() === uri.toString())) {
                    listener.dispose();
                    resolve();
                }
            });
        });

        // Opening the C++ document activates the extension, which starts clice.
        document = await vscode.workspace.openTextDocument(uri);
        await vscode.window.showTextDocument(document);

        const offset = document.getText().indexOf(scenario.symbol);
        assert.ok(offset >= 0, `symbol not found: ${scenario.symbol}`);
        position = document.positionAt(offset);

        await diagnostics;
        // clice offers pulls only to a client declaring them; the extension
        // declines (see declinePullDiagnostics in extension.ts).
        const extension = vscode.extensions.getExtension("clice-io.clice");
        const client = (extension?.exports as { client: ClientHandle }).client;
        assert.strictEqual(
            client.current.initializeResult?.capabilities.diagnosticProvider,
            undefined,
            "diagnostics must stay pushed",
        );
    });

    test("workspace symbol indexed", async function () {
        this.timeout(90 * 1000);

        // Definition is index-based: poll workspace/symbol until the
        // expected symbol shows up, mirroring the integration tests.
        const deadline = Date.now() + 60 * 1000;
        for (;;) {
            const symbols = await vscode.commands.executeCommand<vscode.SymbolInformation[]>(
                "vscode.executeWorkspaceSymbolProvider",
                scenario.indexSymbol,
            );
            if (symbols.some((s) => s.name === scenario.indexSymbol)) {
                return;
            }
            assert.ok(
                Date.now() < deadline,
                `symbol ${scenario.indexSymbol} not indexed within 60s`,
            );
            await new Promise((resolve) => setTimeout(resolve, 1000));
        }
    });

    test("hover", async function () {
        this.timeout(60 * 1000);
        assert.ok(document, "main file was not opened (earlier test failed)");

        const hovers = await vscode.commands.executeCommand<vscode.Hover[]>(
            "vscode.executeHoverProvider",
            document.uri,
            position,
        );
        assert.ok(hovers.length > 0, "hover returned no results");
        assert.ok(hovers[0].contents.length > 0, "hover returned empty contents");
    });

    test("semantic tokens through middleware", async function () {
        this.timeout(60 * 1000);
        assert.ok(document, "main file was not opened (earlier test failed)");

        // The provider path runs through the inactive-regions middleware,
        // so a decode failure or a swallowed response surfaces here as
        // missing tokens. Poll: the provider registers dynamically after
        // the handshake.
        const deadline = Date.now() + 30 * 1000;
        for (;;) {
            const tokens = await vscode.commands.executeCommand<vscode.SemanticTokens | undefined>(
                "vscode.provideDocumentSemanticTokens",
                document.uri,
            );
            if (tokens && tokens.data.length > 0) {
                return;
            }
            assert.ok(
                Date.now() < deadline,
                "no semantic tokens through the middleware within 30s",
            );
            await new Promise((resolve) => setTimeout(resolve, 1000));
        }
    });

    test("definition", async function () {
        this.timeout(60 * 1000);
        if (!scenario.definitionFile) {
            this.skip();
        }
        assert.ok(document, "main file was not opened (earlier test failed)");

        const locations = await vscode.commands.executeCommand<
            (vscode.Location | vscode.LocationLink)[]
        >("vscode.executeDefinitionProvider", document.uri, position);
        assert.ok(locations.length > 0, "definition returned no locations");

        const first = locations[0];
        const target = first instanceof vscode.Location ? first.uri : first.targetUri;
        assert.strictEqual(path.basename(target.fsPath), scenario.definitionFile);
    });

    test("compilation context requests", async function () {
        this.timeout(60 * 1000);
        const folder = workspaceFolder();
        if (path.basename(folder.uri.fsPath) !== "header_context") {
            this.skip();
        }
        assert.ok(document, "main file was not opened (earlier test failed)");

        const extension = vscode.extensions.getExtension("clice-io.clice");
        assert.ok(extension?.isActive, "extension not active");
        const client = (extension.exports as { client: ClientHandle }).client;
        const uri = document.uri.toString();

        const query = await client.sendRequest<QueryContextResult>("clice/queryContext", { uri });
        assert.ok(query.total >= 1, `expected at least one context, got ${query.total}`);
        const host = query.contexts.find((c) => c.uri.includes("main.cpp"));
        assert.ok(host, "main.cpp should be offered as a context");

        // The client contract: a switch through the extension's commands
        // keeps the document open; the server recompiles the unchanged text
        // and publishes its diagnostics anew.
        const published = () =>
            new Promise<void>((resolve, reject) => {
                const timer = setTimeout(() => {
                    subscription.dispose();
                    reject(new Error("no diagnostics publish after the switch"));
                }, 30 * 1000);
                const subscription = vscode.languages.onDidChangeDiagnostics((event) => {
                    if (event.uris.some((changed) => changed.toString() === uri)) {
                        clearTimeout(timer);
                        subscription.dispose();
                        resolve();
                    }
                });
            });

        let republished = published();
        await vscode.commands.executeCommand("clice.applyContext", host, query.epoch, uri);
        await republished;
        const current = await client.sendRequest<CurrentContextResult>("clice/currentContext", {
            uri,
        });
        assert.ok(
            current.context?.uri.includes("main.cpp"),
            "currentContext should report the switched host",
        );
        assert.ok(!current.automatic, "the switched host is the user's choice");
        assert.strictEqual(document.languageId, "cpp", "the switch keeps the document as it was");

        republished = published();
        await vscode.window.showTextDocument(document);
        await vscode.commands.executeCommand("clice.resetContext");
        await republished;
        const automatic = await client.sendRequest<CurrentContextResult>("clice/currentContext", {
            uri,
        });
        assert.ok(automatic.automatic, "the reset leaves the automatic context");
    });

    test("switch source/header without a counterpart", async function () {
        this.timeout(60 * 1000);
        const folder = workspaceFolder();
        if (path.basename(folder.uri.fsPath) !== "header_context") {
            this.skip();
        }
        assert.ok(document, "main file was not opened (earlier test failed)");

        // utils.h defines everything it declares: the command reports that
        // and leaves the editor where it was.
        const extension = vscode.extensions.getExtension("clice-io.clice");
        assert.ok(extension?.isActive, "extension not active");
        const client = (extension.exports as { client: ClientHandle }).client;
        const result = await client.sendRequest<CounterpartsResult>("clice/counterparts", {
            uri: document.uri.toString(),
        });
        assert.deepStrictEqual(result, { candidates: [], preferred: null });
        await vscode.window.showTextDocument(document);
        await vscode.commands.executeCommand("clice.switchSourceHeader");
        assert.strictEqual(
            vscode.window.activeTextEditor?.document.uri.toString(),
            document.uri.toString(),
        );
    });

    test("completion", async function () {
        this.timeout(60 * 1000);
        assert.ok(document, "main file was not opened (earlier test failed)");

        const completions = await vscode.commands.executeCommand<vscode.CompletionList>(
            "vscode.executeCompletionItemProvider",
            document.uri,
            position.translate(0, 1),
        );
        assert.ok(completions.items.length > 0, "completion returned no items");
    });

    test("space trigger gated outside imports", async function () {
        this.timeout(60 * 1000);
        assert.ok(document, "main file was not opened (earlier test failed)");

        // The middleware swallows space-triggered requests on non-import
        // lines; the cursor sits inside ordinary code here. VS Code still
        // contributes word-based suggestions (kind Text), so assert that
        // nothing beyond those — i.e. no server-provided item — shows up.
        const completions = await vscode.commands.executeCommand<vscode.CompletionList>(
            "vscode.executeCompletionItemProvider",
            document.uri,
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
        this.timeout(60 * 1000);
        if (path.basename(workspaceFolder().uri.fsPath) !== "hello_world") {
            this.skip();
        }

        // Each command asks for the kind its name spells: the commands and
        // the refactoring kinds the server advertises are the same set.
        const extension = vscode.extensions.getExtension("clice-io.clice");
        const manifest = extension?.packageJSON as {
            contributes: { commands: { command: string }[] };
        };
        const commands = manifest.contributes.commands
            .map(({ command }) => command)
            .filter((command) => command.startsWith("clice.refactor."))
            .map((command) => command.slice("clice.".length));
        const provider = (extension?.exports as { client: ClientHandle }).client.current
            .initializeResult?.capabilities.codeActionProvider;
        const advertised =
            typeof provider === "object"
                ? (provider.codeActionKinds ?? []).filter((kind) => kind.startsWith("refactor."))
                : [];
        assert.deepStrictEqual([...commands].sort(), [...advertised].sort());

        const root = fs.mkdtempSync(path.join(os.tmpdir(), "clice-refactor-"));
        const file = path.join(root, "paint.cpp");
        fs.writeFileSync(
            file,
            "enum class Color { Red, Green };\n\nint paint(Color color) {\n" +
                "    switch(color) {\n        case Color::Red: return 1;\n    }\n    return 0;\n}\n",
        );
        try {
            const source = await vscode.workspace.openTextDocument(vscode.Uri.file(file));
            const cursor = source.positionAt(source.getText().indexOf("switch"));
            await vscode.window.showTextDocument(source, {
                selection: new vscode.Range(cursor, cursor),
            });
            const edited = new Promise<void>((resolve, reject) => {
                const timer = setTimeout(() => {
                    subscription.dispose();
                    reject(new Error("the command applied no edit"));
                }, 30 * 1000);
                const subscription = vscode.workspace.onDidChangeTextDocument((event) => {
                    if (event.document === source && event.contentChanges.length > 0) {
                        clearTimeout(timer);
                        subscription.dispose();
                        resolve();
                    }
                });
            });
            await Promise.all([
                edited,
                vscode.commands.executeCommand("clice.refactor.rewrite.populateSwitch"),
            ]);
            assert.ok(
                source.getText().includes("case Color::Green:"),
                `the missing case was not added:\n${source.getText()}`,
            );
        } finally {
            await vscode.commands.executeCommand("workbench.action.revertAndCloseActiveEditor");
            fs.rmSync(root, { recursive: true, force: true });
        }
    });
});
