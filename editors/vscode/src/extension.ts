import * as fs from "fs";
import * as net from "net";
import * as path from "path";
import * as vscode from "vscode";
import { workspace, window, ExtensionContext } from "vscode";
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    StreamInfo,
} from "vscode-languageclient/node";
import { getSetting } from "./setting";
import { registerCompilationContext } from "./feature/context";
import { registerInactiveRegions } from "./feature/inactive";

let client: LanguageClient;

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

export async function registerCommands(client: LanguageClient, context: ExtensionContext) {
    context.subscriptions.push(
        vscode.commands.registerCommand("clice.restart", async () => {
            await client.restart();
        }),
    );
}

export async function activate(context: ExtensionContext) {
    console.log('Congratulations, your extension "clice" is now active!');

    const channel = window.createOutputChannel("clice");
    const verboseChannel = window.createOutputChannel("clice-verbose");

    const setting = getSetting();
    if (!setting) {
        return;
    }

    let executable = setting.executable;
    let serverOptions: ServerOptions | (() => Promise<StreamInfo>);

    if (setting.mode === "pipe") {
        if (!executable || executable === "") {
            executable = bundledExecutable(context);
            if (!executable) {
                window.showErrorMessage(
                    "This build of the clice extension does not bundle the clice server; " +
                        "set 'clice.executable' to a locally installed binary.",
                );
                return;
            }
        }

        let args = ["serve"];
        serverOptions = {
            run: { command: executable, args: args },
            debug: { command: executable, args: args },
        };
    } else if (setting.mode === "socket") {
        serverOptions = (): Promise<StreamInfo> => {
            return new Promise((resolve, reject) => {
                const client = new net.Socket();
                client.connect(setting.port, setting.host, () => {
                    resolve({
                        reader: client,
                        writer: client,
                    });
                });
                client.on("error", (error) => {
                    reject(error);
                });
            });
        };
    } else {
        vscode.window.showErrorMessage("Invalid mode, please set the mode to 'pipe' or 'socket'.");
        return;
    }

    const clientOptions: LanguageClientOptions = {
        documentSelector: [
            { scheme: "file", language: "cpp" },
            { scheme: "file", language: "c" },
            { scheme: "file", language: "cuda-cpp" },
        ],
        outputChannel: channel,
        traceOutputChannel: verboseChannel,
        synchronize: {
            fileEvents: workspace.createFileSystemWatcher("**/.clientrc"),
        },
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

    client = new LanguageClient("clice", "clice", serverOptions, clientOptions);

    await registerCommands(client, context);

    await client.start();

    registerCompilationContext(client, context);
    registerInactiveRegions(client, context);

    // Exposed for E2E tests to exercise custom requests directly.
    return { client };
}

export function deactivate(): Thenable<void> | undefined {
    if (!client) {
        return undefined;
    }
    let ret = client.stop();
    return ret;
}
