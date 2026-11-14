import * as path from "path";
import * as vscode from "vscode";
import type { ClientHandle } from "../client";
import type { CompileCommand, CurrentContextResult } from "@clice/tools/protocol" with {
    "resolution-mode": "import",
};

/// Where the command comes from, in a sentence.
function origin(command: CompileCommand): string {
    const from = command.from === null ? "" : path.basename(vscode.Uri.parse(command.from).fsPath);
    switch (command.source) {
        case "database":
            return "an entry of the compilation database";
        case "host":
            return `the command of ${from}, which includes it`;
        case "default":
            return "the default_command of a clice.toml rule";
        case "borrowed":
            return `borrowed from ${from === "" ? "a unit nearby" : from}: no compilation database lists the file`;
        case "fallback":
            return "clice's own: no compilation database lists the file, and no unit lends one";
    }
}

/// An argument as a POSIX shell reads it back.
function quote(argument: string): string {
    return /^[\w@%+=:,./-]+$/.test(argument) ? argument : `'${argument.replaceAll("'", `'\\''`)}'`;
}

/// The command as a script running it, one argument a line.
function script(file: vscode.Uri, command: CompileCommand): string {
    const lines = [`# The command clice compiles ${path.basename(file.fsPath)} with:`];
    lines.push(`# ${origin(command)}.`);
    if (command.queryFailure !== null) {
        lines.push(
            `# clice cannot ask its compiler for its settings: ${command.queryFailure}.`,
            "# clang read the command on its own.",
        );
    }
    lines.push(`cd ${quote(command.directory)}`);
    lines.push(command.arguments.map(quote).join(" \\\n    "));
    return lines.join("\n") + "\n";
}

const scheme = "clice-command";

/// The compile command of the active editor, shown whole in a read-only
/// document.
export function registerCompileCommand(client: ClientHandle, ext: vscode.ExtensionContext) {
    const changed = new vscode.EventEmitter<vscode.Uri>();
    const provider: vscode.TextDocumentContentProvider = {
        onDidChange: changed.event,
        async provideTextDocumentContent(uri) {
            const file = vscode.Uri.parse(uri.query);
            const name = path.basename(file.fsPath);
            try {
                const current = await client.sendRequest<CurrentContextResult>(
                    "clice/currentContext",
                    { uri: file.toString() },
                );
                return current.command === null
                    ? `# ${name} is not open in clice.\n`
                    : script(file, current.command);
            } catch (error) {
                const message = error instanceof Error ? error.message : String(error);
                return `# clice cannot tell the command of ${name}: ${message}\n`;
            }
        },
    };

    async function showCommand() {
        const document = vscode.window.activeTextEditor?.document;
        if (document?.uri.scheme !== "file") {
            return;
        }
        const uri = vscode.Uri.from({
            scheme,
            path: `/${path.basename(document.uri.fsPath)}.sh`,
            query: document.uri.toString(),
        });
        // A document shown before keeps its text until told it changed.
        changed.fire(uri);
        await vscode.window.showTextDocument(await vscode.workspace.openTextDocument(uri), {
            preview: true,
            viewColumn: vscode.ViewColumn.Beside,
        });
    }

    ext.subscriptions.push(
        changed,
        vscode.workspace.registerTextDocumentContentProvider(scheme, provider),
        vscode.commands.registerCommand("clice.showCompileCommand", showCommand),
    );
}
