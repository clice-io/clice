import * as assert from "assert";
import * as cp from "child_process";
import * as fs from "fs";
import { once } from "events";
import * as path from "path";
import { PassThrough } from "stream";
import * as vscode from "vscode";

import type { ClientHandle } from "../client";
import { inactiveSpans } from "../feature/inactive";
import { serverOutput, TraceChannel } from "../log";
import { exited, retire } from "../process";
import { resolveExecutable, serverEnvironment } from "../setting";

// The extension's own tests, which no fixture changes: they run once
// (.vscode-test.mjs). Those of documents a server answers take it from
// CLICE_EXECUTABLE; without it (plain `npm test`) they are skipped.
const executable = process.env.CLICE_EXECUTABLE;

// Before a suite activates the extension, which starts the server.
suiteSetup(async function () {
    if (executable) {
        await vscode.workspace
            .getConfiguration("clice")
            .update("executable", path.resolve(executable), vscode.ConfigurationTarget.Global);
    }
});

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

// Pure decoding of the inactive-modifier spans; no server involved.
// Entries are (deltaLine, deltaStart, length, type, modifiers) tuples.
suite("inactive span decoding", function () {
    const MASK = 1 << 3;

    test("merges consecutive lines into runs", function () {
        const data = [
            ...[1, 0, 3, 0, MASK], // line 1, inactive
            ...[1, 0, 3, 0, MASK], // line 2, inactive
            ...[2, 0, 3, 0, MASK], // line 4: line 3 sent no token, maybe active code
            ...[1, 0, 3, 0, 0], // line 5, active
            ...[1, 0, 3, 0, MASK], // line 6, a separate region
        ];
        assert.deepStrictEqual(inactiveSpans(data, MASK), {
            lines: [
                [1, 2],
                [4, 4],
                [6, 6],
            ],
            tokens: [],
        });
    });

    test("several tokens on one line", function () {
        const data = [...[0, 0, 2, 0, MASK], ...[0, 3, 2, 0, MASK]];
        assert.deepStrictEqual(inactiveSpans(data, MASK), { lines: [[0, 0]], tokens: [] });
    });

    test("arguments dropped on a line that runs code", function () {
        const data = [
            ...[1, 4, 6, 0, 0], // line 1: `return FIRST(left(), right());`
            ...[0, 7, 5, 0, 0],
            ...[0, 6, 4, 0, 0],
            ...[0, 8, 7, 0, MASK], // `right()`, dropped
        ];
        assert.deepStrictEqual(inactiveSpans(data, MASK), { lines: [], tokens: [[1, 25, 7]] });
    });

    test("empty without the modifier", function () {
        const none = { lines: [], tokens: [] };
        assert.deepStrictEqual(inactiveSpans([1, 0, 3, 0, 0], MASK), none);
        assert.deepStrictEqual(inactiveSpans([], MASK), none);
        assert.deepStrictEqual(inactiveSpans([1, 0, 3, 0, MASK], 0), none);
    });
});

suite("editor context menu", function () {
    test("clice's group follows navigation, its commands in order", function () {
        const extension = vscode.extensions.getExtension("clice-io.clice");
        assert.ok(extension, "the extension is installed");
        const menu = (
            extension.packageJSON as {
                contributes: { menus: { "editor/context": { command: string; group?: string }[] } };
            }
        ).contributes.menus["editor/context"];
        assert.deepStrictEqual(
            menu.map((item) => [item.command, item.group]),
            [
                ["clice.switchSourceHeader", "0_clice@1"],
                ["clice.switchContext", "0_clice@2"],
                ["clice.switchConfiguration", "0_clice@3"],
                ["clice.showCompileCommand", "0_clice@4"],
            ],
        );
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

suite("server environment setting", function () {
    test("an override names VS Code's variables", function () {
        const env = serverEnvironment(
            { PATH: "/usr/bin", HOME: "/home/me" },
            { PATH: "/opt/cygwin/bin:${env:PATH}", TMPDIR: "${env:HOME}/tmp${env:UNSET}" },
            false,
        );
        assert.deepStrictEqual(env, {
            PATH: "/opt/cygwin/bin:/usr/bin",
            HOME: "/home/me",
            TMPDIR: "/home/me/tmp",
        });
    });

    test("Windows matches names in any case", function () {
        const env = serverEnvironment(
            { Path: "C:\\Windows", windir: "C:\\Windows" },
            { PATH: "C:\\cygwin64\\bin;${env:path}" },
            true,
        );
        assert.deepStrictEqual(env, {
            PATH: "C:\\cygwin64\\bin;C:\\Windows",
            windir: "C:\\Windows",
        });
    });
});

type Writable<T> = { -readonly [K in keyof T]: T[K] };

/// A message the extension showed: its text and the actions it offered.
interface Message {
    text: string;
    actions: string[];
}

const cpptools = "ms-vscode.cpptools";
const clangd = "llvm-vs-code-extensions.vscode-clangd";

/// The conflict check's warning, naming `names`.
function conflictWarning(names: string): Message {
    return {
        text:
            `clice provides the C/C++ language features, and running ${names} alongside it ` +
            "duplicates completion and diagnostics. Disable the conflicting language features?",
        actions: ["Disable", "Never ask again"],
    };
}

/// Where `section.key` is set: in the user's settings, the workspace's, the
/// folder's.
function scopes(section: string, key: string) {
    const folder = vscode.workspace.workspaceFolders?.[0];
    const inspected = vscode.workspace.getConfiguration(section, folder?.uri).inspect(key);
    return {
        user: inspected?.globalValue,
        workspace: inspected?.workspaceValue,
        folder: inspected?.workspaceFolderValue,
    };
}

// Which other C/C++ extensions are installed, and what the user answers, the
// test says through the API it shares with the extension. Runs before any
// other suite activates the extension: the check hears of extension changes
// through the event it subscribes to at activation.
suite("conflicting extensions", function () {
    /// Fires the extension's `vscode.extensions.onDidChange`, as installing
    /// or enabling an extension does. The check decides before it waits on
    /// anything: once this has fired, a warning it gives has been shown.
    const extensionsChanged = new vscode.EventEmitter<void>();
    const messageShown = new vscode.EventEmitter<void>();
    /// The extensions reported installed beside the real ones.
    let installed: string[] = [];
    /// What the user answers the warning.
    let answer: string | undefined;
    let warnings: Message[] = [];
    let notes: Message[] = [];
    const restore: (() => void)[] = [];

    function client(): ClientHandle {
        const extension = vscode.extensions.getExtension<{ client: ClientHandle }>(
            "clice-io.clice",
        );
        assert.ok(extension?.isActive, "extension not active");
        return extension.exports.client;
    }

    const warned = (count: number) =>
        until([messageShown.event], () => warnings.length === count, `warning ${count}`);

    suiteSetup(async function () {
        this.timeout(60 * 1000);
        if (!executable) {
            if (process.env.CI) {
                throw new Error("CLICE_EXECUTABLE must be set in CI");
            }
            this.skip();
        }
        const extension = vscode.extensions.getExtension("clice-io.clice");
        assert.ok(
            extension && !extension.isActive,
            "the extension started before the test could announce extension changes to it",
        );
        const extensions = vscode.extensions;
        const descriptor = Object.getOwnPropertyDescriptor(extensions, "onDidChange");
        assert.ok(descriptor, "vscode.extensions has its own onDidChange");
        const changed = extensions.onDidChange;
        const announced: vscode.Event<void> = (listener, thisArgs, disposables) => {
            const both = vscode.Disposable.from(
                changed(listener, thisArgs),
                extensionsChanged.event(listener, thisArgs),
            );
            disposables?.push(both);
            return both;
        };
        Object.defineProperty(extensions, "onDidChange", {
            configurable: true,
            get: () => announced,
        });
        try {
            await extension.activate();
        } finally {
            Object.defineProperty(extensions, "onDidChange", descriptor);
        }
        assert.ok(client().isRunning(), "clice runs");
    });

    suiteTeardown(function () {
        extensionsChanged.dispose();
        messageShown.dispose();
    });

    setup(function () {
        installed = [];
        answer = undefined;
        warnings = [];
        notes = [];
        const extensions = vscode.extensions as Writable<typeof vscode.extensions>;
        const window = vscode.window as Writable<typeof vscode.window>;
        const { getExtension } = extensions;
        const { showWarningMessage, showInformationMessage } = window;
        extensions.getExtension = ((id: string) =>
            installed.includes(id)
                ? ({ id } as vscode.Extension<unknown>)
                : getExtension(id)) as typeof getExtension;
        const record =
            (shown: Message[], reply: () => string | undefined) =>
            (text: string, ...actions: string[]) => {
                shown.push({ text, actions });
                messageShown.fire();
                return Promise.resolve(reply());
            };
        window.showWarningMessage = record(
            warnings,
            () => answer,
        ) as unknown as typeof showWarningMessage;
        // Dismissed: the offer to reload would reload the window under test.
        window.showInformationMessage = record(
            notes,
            () => undefined,
        ) as unknown as typeof showInformationMessage;
        restore.push(() => {
            extensions.getExtension = getExtension;
            window.showWarningMessage = showWarningMessage;
            window.showInformationMessage = showInformationMessage;
        });
    });

    teardown(async function () {
        for (const each of restore.splice(0)) {
            each();
        }
        for (const [section, key] of [
            ["clangd", "enable"],
            ["C_Cpp", "intelliSenseEngine"],
        ]) {
            const config = vscode.workspace.getConfiguration(section);
            await config.update(key, undefined, vscode.ConfigurationTarget.Global);
            await config.update(key, undefined, vscode.ConfigurationTarget.Workspace);
        }
    });

    test("named once as clice starts, and turned off on Disable", async function () {
        this.timeout(120 * 1000);
        // As a repository's .vscode/settings.json does, which outranks the
        // user's settings.
        await vscode.workspace
            .getConfiguration("clangd")
            .update("enable", true, vscode.ConfigurationTarget.Workspace);
        installed = [cpptools, clangd];
        answer = "Disable";
        await vscode.commands.executeCommand("clice.restart");
        await until([messageShown.event], () => notes.length > 0, "the offer to reload");
        assert.deepStrictEqual(warnings, [conflictWarning("C/C++ (cpptools), clangd")]);
        assert.deepStrictEqual(notes, [
            {
                text: "clice: conflicting language features disabled — reload the window to fully apply.",
                actions: ["Reload Window"],
            },
        ]);
        assert.deepStrictEqual(scopes("clangd", "enable"), {
            user: false,
            workspace: undefined,
            folder: undefined,
        });
        assert.deepStrictEqual(scopes("C_Cpp", "intelliSenseEngine"), {
            user: "disabled",
            workspace: undefined,
            folder: undefined,
        });
    });

    test("an installed one with its language features off is none", async function () {
        const user = vscode.ConfigurationTarget.Global;
        // cpptools kept for its debugger.
        await vscode.workspace
            .getConfiguration("C_Cpp")
            .update("intelliSenseEngine", "disabled", user);
        await vscode.workspace.getConfiguration("clangd").update("enable", false, user);
        installed = [cpptools, clangd];
        extensionsChanged.fire();
        assert.deepStrictEqual(warnings, []);

        await vscode.workspace
            .getConfiguration("C_Cpp")
            .update("intelliSenseEngine", "default", user);
        extensionsChanged.fire();
        await warned(1);
        assert.deepStrictEqual(warnings, [conflictWarning("C/C++ (cpptools)")]);
        assert.deepStrictEqual(notes, []);
    });

    test("no warning while clice is not running", async function () {
        this.timeout(120 * 1000);
        installed = [clangd];
        await client().current.stop();
        extensionsChanged.fire();
        assert.deepStrictEqual(warnings, []);

        await vscode.commands.executeCommand("clice.restart");
        await warned(1);
        assert.deepStrictEqual(warnings, [conflictWarning("clangd")]);
    });

    // Last: the choice holds for the rest of the run.
    test("Never ask again stops the warnings", async function () {
        this.timeout(120 * 1000);
        installed = [clangd];
        answer = "Never ask again";
        extensionsChanged.fire();
        await warned(1);
        await vscode.commands.executeCommand("clice.restart");
        extensionsChanged.fire();
        assert.deepStrictEqual(warnings, [conflictWarning("clangd")]);
        assert.deepStrictEqual(notes, []);
        assert.deepStrictEqual(scopes("clangd", "enable"), {
            user: undefined,
            workspace: undefined,
            folder: undefined,
        });
    });
});

/// A new folder outside the workspace, which .vscode-test.mjs removes once
/// VS Code has exited.
function scratchFolder(prefix: string): string {
    const scratch = process.env.CLICE_E2E_SCRATCH;
    assert.ok(scratch, "CLICE_E2E_SCRATCH names the folder .vscode-test.mjs made");
    return fs.mkdtempSync(path.join(scratch, prefix));
}

suite("second names", function () {
    test("an editor on a second name moves to the open document", async function () {
        this.timeout(90 * 1000);
        const root = scratchFolder("clice-alias-");
        fs.mkdirSync(path.join(root, "real"));
        fs.writeFileSync(path.join(root, "real", "a.cpp"), "int a() { return 0; }\n");
        fs.symlinkSync(path.join(root, "real"), path.join(root, "link"));
        const secondUri = vscode.Uri.file(path.join(root, "link", "a.cpp"));
        // Activated first, the extension hears of the second name's editor
        // first; then the host stays busy while the selection the open
        // asked for arrives, as a loaded CI runner's does.
        await vscode.extensions.getExtension("clice-io.clice")?.activate();
        const busy = vscode.window.onDidChangeActiveTextEditor((editor) => {
            if (editor?.document.uri.toString() === secondUri.toString()) {
                const until = Date.now() + 500;
                while (Date.now() < until) {
                    // Busy.
                }
            }
        });
        try {
            const first = await vscode.workspace.openTextDocument(
                vscode.Uri.file(path.join(root, "real", "a.cpp")),
            );
            await vscode.window.showTextDocument(first);
            const second = await vscode.workspace.openTextDocument(secondUri);
            await vscode.window.showTextDocument(second, {
                selection: new vscode.Range(0, 4, 0, 5),
                preview: false,
            });
            const tabs = () =>
                vscode.window.tabGroups.all
                    .flatMap((group) => group.tabs)
                    .filter((tab) => tab.input instanceof vscode.TabInputText)
                    .map((tab) => (tab.input as vscode.TabInputText).uri.toString());
            // The redirect shows the first document, then closes the
            // second name's tab.
            await until<unknown>(
                [
                    vscode.window.tabGroups.onDidChangeTabs,
                    vscode.window.onDidChangeActiveTextEditor,
                ],
                () =>
                    !tabs().includes(second.uri.toString()) &&
                    vscode.window.activeTextEditor?.document === first,
                "redirect to the open document",
            );
            const active = vscode.window.activeTextEditor;
            assert.ok(active);
            assert.deepStrictEqual(active.selection.start, new vscode.Position(0, 4));
        } finally {
            busy.dispose();
            await vscode.commands.executeCommand("workbench.action.closeAllEditors");
        }
    });
});
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
        const server = stub(
            "process.on('SIGTERM', () => {}); console.log('ready'); setInterval(() => {}, 1000)",
        );
        // Its SIGTERM handler is in place once it says so.
        await once(server.stdout, "data");
        const lines: string[] = [];
        await retire(server, { exit: 2_000, term: 500 }, (line) => {
            lines.push(line);
        });
        assert.ok(exited(server));
        assert.strictEqual(lines.length, process.platform === "win32" ? 1 : 2);
    });
});

suite("server output", function () {
    test("each line at its level, a stack trace with the line before", async function () {
        const shown: [string, string][] = [];
        const record = (level: string) => (text: string) => shown.push([level, text]);
        const channel = {
            trace: record("trace"),
            debug: record("debug"),
            info: record("info"),
            warn: record("warn"),
            error: record("error"),
        } as unknown as vscode.LogOutputChannel;
        const input = new PassThrough();
        serverOutput.stderr(input, channel);
        input.end(
            [
                "[2026-11-10 03:28:05.123] [info] [compile.cpp:555] Compiled main.cpp",
                "[2026-11-10 03:28:05.124] [warning] [thread 2] [pool.cpp:39] slow",
                "[2026-11-10 03:28:05.125] [fatal] clice crashed",
                "#0 0x1234 in main",
                "",
            ].join("\n"),
        );
        await once(input, "end");
        assert.deepStrictEqual(shown, [
            ["info", "[compile.cpp:555] Compiled main.cpp"],
            ["warn", "[thread 2] [pool.cpp:39] slow"],
            ["error", "clice crashed"],
            ["error", "#0 0x1234 in main"],
        ]);
    });
});

suite("trace channel", function () {
    test("traces as clice.trace.server says", async function () {
        const settings = vscode.workspace.getConfiguration("clice");
        const channel = new TraceChannel();
        try {
            assert.strictEqual(channel.logLevel, vscode.LogLevel.Off);
            const changed = new Promise<vscode.LogLevel>((resolve) => {
                channel.onDidChangeLogLevel(resolve);
            });
            await settings.update("trace.server", "verbose", vscode.ConfigurationTarget.Global);
            assert.strictEqual(await changed, vscode.LogLevel.Trace);
        } finally {
            await settings.update("trace.server", undefined, vscode.ConfigurationTarget.Global);
            channel.dispose();
        }
    });
});

suite("diagnostics of a tab", function () {
    suiteSetup(function () {
        if (!executable) {
            if (process.env.CI) {
                throw new Error("CLICE_EXECUTABLE must be set in CI");
            }
            this.skip();
        }
    });

    test("a closed tab leaves no problems", async function () {
        this.timeout(90 * 1000);
        const root = scratchFolder("clice-problems-");
        const uri = vscode.Uri.file(path.join(root, "broken.cpp"));
        fs.writeFileSync(uri.fsPath, "int value = missing;\n");
        const problems = () => vscode.languages.getDiagnostics(uri).length;
        const listed = (ready: () => boolean, what: string) =>
            until([vscode.languages.onDidChangeDiagnostics], ready, what);
        try {
            // Opened through the API, as GitHub Pull Requests has every file
            // opened: VS Code keeps such a document open for a while after
            // its tab closes.
            const document = await vscode.workspace.openTextDocument(uri);
            await vscode.window.showTextDocument(document, { preview: false });
            await listed(() => problems() > 0, "problems of the open tab");
            await vscode.commands.executeCommand("workbench.action.closeActiveEditor");
            assert.ok(!document.isClosed, "the document outlives its tab");
            await listed(() => problems() === 0, "problems gone with the tab");
            await vscode.window.showTextDocument(document, { preview: false });
            await listed(() => problems() > 0, "problems back with the tab");
        } finally {
            await vscode.commands.executeCommand("workbench.action.closeAllEditors");
        }
    });
});
