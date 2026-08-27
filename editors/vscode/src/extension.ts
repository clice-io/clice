import * as cp from "child_process";
import * as fs from "fs";
import * as net from "net";
import * as path from "path";
import * as vscode from "vscode";
import { window, ExtensionContext } from "vscode";
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    StreamInfo,
} from "vscode-languageclient/node";
import { getSetting, Setting } from "./setting";
import { registerCompilationContext } from "./feature/context";
import { registerConflictCheck } from "./feature/conflicts";
import { registerInactiveRegions } from "./feature/inactive";

let client: LanguageClient | undefined;

// Platform-specific builds of the extension ship the server under clice/;
// universal builds (a plain `vsce package` without the binary staged) carry
// none and require the clice.executable setting instead.
function bundledExecutable(context: ExtensionContext): string | undefined {
    const name = process.platform === "win32" ? "clice.exe" : "clice";
    const bundled = context.asAbsolutePath(path.join("clice", "bin", name));
    if (!fs.existsSync(bundled)) {
        return undefined;
    }
    if (process.platform !== "win32") {
        // VSIX extraction may drop the unix executable bit; restore it.
        try {
            fs.chmodSync(bundled, 0o755);
        } catch {
            // Best effort: if chmod fails, spawn will surface the error.
        }
    }
    return bundled;
}

function connectSocket(host: string, port: number): Promise<net.Socket> {
    return new Promise((resolve, reject) => {
        const socket = net.connect(port, host);
        const fail = (message: string) => {
            socket.destroy();
            reject(new Error(message));
        };
        socket.setTimeout(5000, () => {
            fail(`timed out connecting to clice at ${host}:${port}`);
        });
        socket.on("error", (error) => {
            fail(`cannot connect to clice at ${host}:${port}: ${error.message}`);
        });
        socket.on("connect", () => {
            socket.setTimeout(0);
            resolve(socket);
        });
    });
}

/// Connected by validateSettings and consumed by the server-options
/// factory. The server dedicates its LSP slot to the first accepted
/// connection and never reclaims it, so the reachability check must BE
/// the connection — a probe that disconnects would burn the slot.
let pendingSocket: net.Socket | undefined;

// Settings are read fresh on every (re)start so a plain server restart picks
// them up. A rejection here is terminal for the LanguageClient instance
// (StartFailed keeps the rejected start promise forever), so everything
// recoverable is validated in startServer before start() is ever called.
function makeServerOptions(
    context: ExtensionContext,
    channel: vscode.OutputChannel,
): ServerOptions {
    let child: cp.ChildProcess | undefined;
    return async (): Promise<StreamInfo | cp.ChildProcess> => {
        const setting = getSetting();
        if (setting.mode === "socket") {
            const socket = pendingSocket;
            pendingSocket = undefined;
            if (socket && !socket.destroyed) {
                return { reader: socket, writer: socket };
            }
            // Library-initiated restart after a crash: no validation ran.
            const fresh = await connectSocket(setting.host, setting.port);
            return { reader: fresh, writer: fresh };
        }
        const executable = setting.executable ?? bundledExecutable(context);
        if (!executable) {
            throw new Error("no clice executable available");
        }
        if (child) {
            const previous = child;
            // Signal deaths leave exitCode null and set signalCode only.
            const exited = () => previous.exitCode !== null || previous.signalCode !== null;
            if (!exited()) {
                // The LSP exit already told the old server to quit, and
                // quitting persists its caches — give it time, and kill
                // only a genuinely hung server (the library force-kills
                // only processes it spawned itself).
                await new Promise<void>((resolve) => {
                    const timer = setTimeout(() => {
                        previous.kill();
                        resolve();
                    }, 10_000);
                    const finish = () => {
                        clearTimeout(timer);
                        resolve();
                    };
                    previous.once("exit", finish);
                    if (exited()) {
                        finish();
                    }
                });
            }
        }
        child = cp.spawn(executable, ["serve"]);
        child.on("error", (error) => {
            channel.appendLine(`clice spawn failed: ${error.message}`);
        });
        return child;
    };
}

async function validateSettings(context: ExtensionContext): Promise<string | undefined> {
    let setting: Setting;
    try {
        setting = getSetting();
    } catch (error) {
        return error instanceof Error ? error.message : String(error);
    }
    if (setting.mode === "socket") {
        pendingSocket?.destroy();
        pendingSocket = undefined;
        try {
            pendingSocket = await connectSocket(setting.host, setting.port);
            return undefined;
        } catch (error) {
            return error instanceof Error ? error.message : String(error);
        }
    }
    const executable = setting.executable ?? bundledExecutable(context);
    if (!executable) {
        return (
            "This build of the clice extension does not bundle the clice server; " +
            "set 'clice.executable' to a locally installed binary."
        );
    }
    try {
        if (!fs.statSync(executable).isFile()) {
            return `clice executable is not a file: ${executable}`;
        }
        fs.accessSync(executable, fs.constants.X_OK);
    } catch {
        return `clice executable not found or not executable: ${executable} — check 'clice.executable'.`;
    }
    return undefined;
}

/// A startServer pass is in flight / another was requested meanwhile. A
/// restart during a slow start would otherwise no-op: start() on a
/// Starting client returns the existing start promise, never re-running
/// the factory with the new settings.
let restarting = false;
let restartQueued = false;

function takeQueuedRestart(): boolean {
    const queued = restartQueued;
    restartQueued = false;
    return queued;
}

// Start, or restart when running. Serves initial activation, the restart
// command, and the settings-changed prompt alike, so a user can always
// recover from a bad configuration without reloading the window.
async function startServer(context: ExtensionContext): Promise<void> {
    if (!client) {
        return;
    }
    if (restarting) {
        restartQueued = true;
        return;
    }
    restarting = true;
    try {
        do {
            // Stop before validating: the LSP shutdown tells the server to
            // exit (in socket mode that is the user's external server), so
            // validation must look at what is reachable afterwards.
            try {
                if (client.isRunning()) {
                    await client.stop();
                }
            } catch {
                // Stopping a hung server times out; start fresh regardless.
            }
            const problem = await validateSettings(context);
            if (problem) {
                void window.showErrorMessage(`clice: ${problem}`);
                continue;
            }
            try {
                await client.start();
            } catch (error) {
                const message = error instanceof Error ? error.message : String(error);
                void window.showErrorMessage(`clice failed to start: ${message}`);
            } finally {
                pendingSocket?.destroy();
                pendingSocket = undefined;
            }
        } while (takeQueuedRestart());
    } finally {
        restarting = false;
    }
}

async function onSettingsChanged(
    event: vscode.ConfigurationChangeEvent,
    context: ExtensionContext,
) {
    const launchSettings = ["clice.executable", "clice.mode", "clice.host", "clice.port"];
    if (!launchSettings.some((setting) => event.affectsConfiguration(setting))) {
        return;
    }
    const choice = await window.showInformationMessage(
        "clice server settings changed.",
        "Restart Server",
    );
    if (choice === "Restart Server") {
        await startServer(context);
    }
}

export async function activate(context: ExtensionContext) {
    const channel = window.createOutputChannel("clice");
    const traceChannel = window.createOutputChannel("clice (LSP trace)");
    context.subscriptions.push(channel, traceChannel);

    const clientOptions: LanguageClientOptions = {
        documentSelector: [
            { scheme: "file", language: "cpp" },
            { scheme: "file", language: "c" },
            { scheme: "file", language: "cuda-cpp" },
        ],
        outputChannel: channel,
        traceOutputChannel: traceChannel,
        middleware: {
            // Space triggers exist only for `import ` module completion.
            // This guard is intentionally stricter than the server-side
            // detection (exact single-space forms only): it merely avoids
            // request round-trips, while the server independently answers
            // space triggers outside import contexts with an empty list.
            provideCompletionItem: async (document, position, context, token, next) => {
                if (context.triggerCharacter === " ") {
                    const line = document.lineAt(position.line).text.slice(0, position.character);
                    const trimmed = line.trimStart();
                    if (trimmed !== "import " && trimmed !== "export import ") {
                        return [];
                    }
                }
                return next(document, position, context, token);
            },
        },
    };

    client = new LanguageClient(
        "clice",
        "clice",
        makeServerOptions(context, channel),
        clientOptions,
    );

    context.subscriptions.push(
        vscode.commands.registerCommand("clice.restart", () => startServer(context)),
        vscode.workspace.onDidChangeConfiguration((event) => {
            void onSettingsChanged(event, context);
        }),
    );

    registerCompilationContext(client, context);
    registerInactiveRegions(client, context);
    registerConflictCheck(client, context);

    await startServer(context);

    // Exposed for E2E tests to exercise custom requests directly.
    return { client };
}

export function deactivate(): Thenable<void> | undefined {
    if (!client?.isRunning()) {
        return undefined;
    }
    return client.stop();
}
