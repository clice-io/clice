import * as path from "path";
import * as vscode from "vscode";
import { State } from "vscode-languageclient/node";
import type { ClientHandle } from "../client";
import type { CompileCommandResult } from "@clice/tools/protocol" with {
    "resolution-mode": "import",
};
import { LanguageStatus } from "./status";

function isCppEditor(editor: vscode.TextEditor | undefined): editor is vscode.TextEditor {
    const language = editor?.document.languageId;
    return (
        editor?.document.uri.scheme === "file" &&
        (language === "c" || language === "cpp" || language === "cuda-cpp")
    );
}

/// Where the command comes from, in a sentence.
function origin(result: CompileCommandResult): string {
    const from = result.from === null ? "" : path.basename(vscode.Uri.parse(result.from).fsPath);
    switch (result.source) {
        case "database":
            return "an entry of the compilation database";
        case "host":
            return `the command of ${from}, which includes it`;
        case "default":
            return "the default_command of a clice.toml rule";
        case "inferred":
            return `borrowed from ${from}: no compilation database lists the file`;
        case "fallback":
            return "clice's own: no compilation database lists the file, and no unit lends one";
    }
}

/// An argument as a POSIX shell reads it back.
function quote(argument: string): string {
    return /^[\w@%+=:,./-]+$/.test(argument) ? argument : `'${argument.replaceAll("'", `'\\''`)}'`;
}

/// The command as a script running it, one argument a line.
function script(file: vscode.Uri, result: CompileCommandResult): string {
    const lines = [`# The command clice compiles ${path.basename(file.fsPath)} with:`];
    lines.push(`# ${origin(result)}.`);
    if (result.queryFailure !== null) {
        lines.push(
            `# Its compiler could not be queried (${result.queryFailure}), so clang read the command on its own.`,
        );
    }
    lines.push(`cd ${quote(result.directory)}`);
    lines.push(result.arguments.map(quote).join(" \\\n    "));
    return lines.join("\n") + "\n";
}

const scheme = "clice-command";

/// The compile command of the active editor: a language status item saying
/// where it comes from, and a read-only document showing it whole.
export function registerCompileCommand(client: ClientHandle, ext: vscode.ExtensionContext) {
    const status = new LanguageStatus("clice.compileCommand", "clice: compile command");
    const show: vscode.Command = { title: "Show", command: "clice.showCompileCommand" };

    const changed = new vscode.EventEmitter<vscode.Uri>();
    const provider: vscode.TextDocumentContentProvider = {
        onDidChange: changed.event,
        async provideTextDocumentContent(uri) {
            const file = vscode.Uri.parse(uri.query);
            try {
                const result = await client.sendRequest<CompileCommandResult>(
                    "clice/compileCommand",
                    { uri: file.toString() },
                );
                return script(file, result);
            } catch (error) {
                const message = error instanceof Error ? error.message : String(error);
                return `# clice cannot tell the command of ${path.basename(file.fsPath)}: ${message}\n`;
            }
        },
    };

    /// Bumped by every refresh; a reply is dropped when a newer request
    /// started while it was in flight.
    let generation = 0;

    async function refresh(editor: vscode.TextEditor | undefined) {
        generation += 1;
        const current = generation;
        if (!isCppEditor(editor)) {
            return;
        }
        let result: CompileCommandResult | undefined;
        try {
            result = await client.sendRequest<CompileCommandResult>("clice/compileCommand", {
                uri: editor.document.uri.toString(),
            });
        } catch {
            result = undefined;
        }
        if (current !== generation) {
            return;
        }
        if (!result) {
            status.hide();
            return;
        }
        const failed =
            result.queryFailure === null
                ? ""
                : `; its compiler could not be queried: ${result.queryFailure}`;
        status.show(
            result.source,
            origin(result) + failed,
            show,
            result.source === "fallback" || result.queryFailure !== null,
        );
    }

    async function showCommand() {
        const editor = vscode.window.activeTextEditor;
        if (!isCppEditor(editor)) {
            return;
        }
        const uri = vscode.Uri.from({
            scheme,
            path: `/${path.basename(editor.document.uri.fsPath)}.sh`,
            query: editor.document.uri.toString(),
        });
        // A document shown before keeps its text until told it changed.
        changed.fire(uri);
        const document = await vscode.workspace.openTextDocument(uri);
        await vscode.window.showTextDocument(document, {
            preview: true,
            viewColumn: vscode.ViewColumn.Beside,
        });
    }

    ext.subscriptions.push(
        status,
        changed,
        vscode.workspace.registerTextDocumentContentProvider(scheme, provider),
        vscode.commands.registerCommand("clice.showCompileCommand", showCommand),
        vscode.window.onDidChangeActiveTextEditor((editor) => void refresh(editor)),
        // A compile of the active file follows a change of its command: a
        // context switch, a database or rule edit, a save making it a unit.
        vscode.languages.onDidChangeDiagnostics((event) => {
            const editor = vscode.window.activeTextEditor;
            if (
                editor &&
                event.uris.some((uri) => uri.toString() === editor.document.uri.toString())
            ) {
                void refresh(editor);
            }
        }),
        client.onDidChangeState((event) => {
            if (event.newState !== State.Starting) {
                void refresh(vscode.window.activeTextEditor);
            }
        }),
    );
}
