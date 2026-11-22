import * as vscode from "vscode";
import { enabled, workspaceTarget } from "./setting";

async function sayDisabled() {
    const choice = await vscode.window.showInformationMessage(
        "clice is disabled by the 'clice.enable' setting.",
        "Enable",
    );
    if (choice === "Enable") {
        await vscode.workspace.getConfiguration("clice").update("enable", true, workspaceTarget());
    }
}

/// One of the commands the manifest contributes, which while clice.enable is
/// off says so and offers to turn clice on, instead of running.
export function registerCommand(
    command: string,
    run: Parameters<typeof vscode.commands.registerCommand>[1],
): vscode.Disposable {
    return vscode.commands.registerCommand(command, (...args: unknown[]): unknown =>
        enabled() ? run(...args) : sayDisabled(),
    );
}
