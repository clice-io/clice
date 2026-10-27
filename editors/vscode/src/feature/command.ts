import * as path from "path";
import * as vscode from "vscode";
import type { ClientHandle } from "../client";
import type { CompileCommand, CurrentContextResult } from "@clice/tools/protocol" with {
    "resolution-mode": "import",
};
import { LanguageStatus } from "./status";

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
        case "inferred":
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
            `# Its compiler could not be queried (${command.queryFailure}), so clang read the command on its own.`,
        );
    }
    lines.push(`cd ${quote(command.directory)}`);
    lines.push(command.arguments.map(quote).join(" \\\n    "));
    return lines.join("\n") + "\n";
}

const scheme = "clice-command";

/// The compile command of the active editor: a language status item saying
/// where it comes from, and a read-only document showing it whole. Returns
/// what shows the command of the active editor's current context, which the
/// compilation context feature fetches; null hides it.
export function registerCompileCommand(
    client: ClientHandle,
    ext: vscode.ExtensionContext,
): (command: CompileCommand | null) => void {
    const status = new LanguageStatus("clice.compileCommand", "clice: compile command");
    const show: vscode.Command = { title: "Show", command: "clice.showCompileCommand" };

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
        status,
        changed,
        vscode.workspace.registerTextDocumentContentProvider(scheme, provider),
        vscode.commands.registerCommand("clice.showCompileCommand", showCommand),
    );

    return (command) => {
        if (command === null) {
            status.hide();
            return;
        }
        const failed =
            command.queryFailure === null
                ? ""
                : `; its compiler could not be queried: ${command.queryFailure}`;
        status.show(
            command.source,
            origin(command) + failed,
            show,
            command.source === "fallback" || command.queryFailure !== null,
        );
    };
}
