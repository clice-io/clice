import * as vscode from "vscode";

/// The refactoring commands the manifest contributes, each named after the
/// code action kind it asks for (`clice.refactor.rewrite.populateSwitch`
/// asks for `refactor.rewrite.populateSwitch`): the editor requests that
/// kind alone and applies the action directly when it is the only one.
export function registerRefactorCommands(context: vscode.ExtensionContext): void {
    const manifest = context.extension.packageJSON as {
        contributes: { commands: { command: string }[] };
    };
    for (const { command } of manifest.contributes.commands) {
        if (!command.startsWith("clice.refactor.")) {
            continue;
        }
        const kind = command.slice("clice.".length);
        context.subscriptions.push(
            vscode.commands.registerCommand(command, () =>
                vscode.commands.executeCommand("editor.action.codeAction", {
                    kind,
                    apply: "ifSingle",
                }),
            ),
        );
    }
}
