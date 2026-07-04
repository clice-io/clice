import * as assert from "assert";
import * as path from "path";
import * as vscode from "vscode";

// E2E smoke tests against a real clice binary. The binary path comes from
// CLICE_EXECUTABLE; without it (plain `pnpm test`) the suite is skipped.
// The workspace folder is set by .vscode-test.mjs and selects the scenario.

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

suite("clice E2E", function () {
    const executable = process.env.CLICE_EXECUTABLE;
    if (!executable) {
        if (process.env.CI) {
            throw new Error("CLICE_EXECUTABLE must be set in CI");
        }
        return;
    }

    let document: vscode.TextDocument;
    let position: vscode.Position;
    let scenario: Scenario;

    suiteSetup(async function () {
        this.timeout(60 * 1000);

        const folder = vscode.workspace.workspaceFolders?.[0];
        assert.ok(folder, "no workspace folder");
        scenario = scenarios[path.basename(folder.uri.fsPath)];
        assert.ok(scenario, `no scenario for workspace ${folder.uri.fsPath}`);

        await vscode.workspace
            .getConfiguration("clice")
            .update("executable", path.resolve(executable), vscode.ConfigurationTarget.Global);
    });

    test("server starts and publishes diagnostics", async function () {
        this.timeout(240 * 1000);

        const folder = vscode.workspace.workspaceFolders![0];
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
            if ((symbols ?? []).some((s) => s.name === scenario.indexSymbol)) {
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
        const folder = vscode.workspace.workspaceFolders![0];
        if (path.basename(folder.uri.fsPath) !== "header_context") {
            this.skip();
        }
        assert.ok(document, "main file was not opened (earlier test failed)");

        const extension = vscode.extensions.getExtension("ykiko.clice-vscode");
        assert.ok(extension?.isActive, "extension not active");
        const client = extension.exports.client;
        const uri = document.uri.toString();

        const query = await client.sendRequest("clice/queryContext", { uri });
        assert.ok(query.total >= 1, `expected at least one context, got ${query.total}`);
        const host = query.contexts.find((c: { uri: string }) => c.uri.includes("main.cpp"));
        assert.ok(host, "main.cpp should be offered as a context");

        const switched = await client.sendRequest("clice/switchContext", {
            uri,
            contextUri: host.uri,
        });
        assert.ok(switched.success, "switchContext should succeed");

        const current = await client.sendRequest("clice/currentContext", { uri });
        assert.ok(
            current.context && current.context.uri.includes("main.cpp"),
            "currentContext should report the switched host",
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
});
