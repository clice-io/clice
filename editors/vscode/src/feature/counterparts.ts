import * as path from "path";
import * as vscode from "vscode";
import type { ClientHandle } from "../client";
import type { CounterpartsResult } from "@clice/tools/protocol" with {
    "resolution-mode": "import",
};

/** Switch Source/Header: open the file the active one pairs with — at once
 * when the server names one candidate decisive, otherwise through a quick
 * pick listing every candidate with what pairs it. */
export function registerCounterparts(client: ClientHandle, ext: vscode.ExtensionContext) {
    async function switchSourceHeader() {
        const document = vscode.window.activeTextEditor?.document;
        if (document?.uri.scheme !== "file") {
            return;
        }
        let result: CounterpartsResult;
        try {
            result = await client.sendRequest<CounterpartsResult>("clice/counterparts", {
                uri: document.uri.toString(),
            });
        } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            vscode.window.showWarningMessage(`clice: cannot find counterparts: ${message}`);
            return;
        }
        if (result.preferred !== null) {
            await vscode.window.showTextDocument(vscode.Uri.parse(result.preferred));
            return;
        }
        if (result.candidates.length === 0) {
            vscode.window.showInformationMessage(
                `clice: no counterpart of ${path.basename(document.uri.fsPath)} found`,
            );
            return;
        }
        const items = result.candidates.map((candidate) => {
            const uri = vscode.Uri.parse(candidate.uri);
            return {
                label: path.basename(uri.fsPath),
                description: vscode.workspace.asRelativePath(uri),
                detail: candidate.reasons.join(", "),
                uri,
            };
        });
        const chosen = await vscode.window.showQuickPick(items, {
            title: "Switch Source/Header",
            placeHolder: "File to open",
            matchOnDescription: true,
        });
        if (chosen) {
            await vscode.window.showTextDocument(chosen.uri);
        }
    }

    ext.subscriptions.push(
        vscode.commands.registerCommand("clice.switchSourceHeader", switchSourceHeader),
    );
}
