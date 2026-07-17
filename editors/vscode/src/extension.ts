import * as net from "net";
import * as vscode from "vscode";
import { workspace, window, ExtensionContext } from "vscode";
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    StreamInfo,
} from "vscode-languageclient/node";
import { getSetting } from "./setting";
import { ensureServerBinary } from "./download";
import { registerCompilationContext } from "./feature/context";
import { registerInactiveRegions } from "./feature/inactive";

let client: LanguageClient;

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
            const downloadedPath = await ensureServerBinary(context, channel);
            if (downloadedPath) {
                executable = downloadedPath;
            } else {
                window.showErrorMessage("Could not find or download clice executable.");
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
