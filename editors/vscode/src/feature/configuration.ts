import * as vscode from "vscode";
import { State } from "vscode-languageclient/node";
import type { ClientHandle } from "../client";
import type {
    ListConfigurationsResult,
    SwitchConfigurationResult,
} from "@clice/tools/protocol" with { "resolution-mode": "import" };

/** The build configuration switcher: a status bar item showing the
 * configuration the server runs (only while the rules declare any) and a
 * quick pick that persists a new choice and restarts the server, since a
 * selection takes effect at the next start. */
export function registerBuildConfiguration(client: ClientHandle, ext: vscode.ExtensionContext) {
    const status = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 101);
    status.command = "clice.switchConfiguration";
    status.tooltip = "clice: active build configuration (click to switch)";

    /// Bumped by every refresh; a response is dropped when a newer request
    /// started while it was in flight.
    let generation = 0;

    async function refresh() {
        generation += 1;
        const current = generation;
        let result: ListConfigurationsResult;
        try {
            result = await client.sendRequest<ListConfigurationsResult>(
                "clice/listConfigurations",
                {},
            );
        } catch {
            // Server not ready; nothing to show.
            if (current === generation) {
                status.hide();
            }
            return;
        }
        if (current !== generation) {
            return;
        }
        if (result.configurations.length === 0) {
            status.hide();
            return;
        }
        status.text = `$(settings-gear) ${result.active}`;
        status.show();
    }

    async function select() {
        let result: ListConfigurationsResult;
        try {
            result = await client.sendRequest<ListConfigurationsResult>(
                "clice/listConfigurations",
                {},
            );
        } catch {
            vscode.window.showWarningMessage("clice: server not ready");
            return;
        }
        if (result.configurations.length === 0) {
            vscode.window.showInformationMessage(
                "clice: the rules declare no build configurations to switch between",
            );
            return;
        }
        const items = result.configurations.map((name) => {
            const notes: string[] = [];
            if (name === result.active) {
                notes.push("active");
            }
            if (name === result.defaultConfiguration) {
                notes.push("default");
            }
            return { label: name, description: notes.join(", ") };
        });
        const chosen = await vscode.window.showQuickPick(items, {
            title: "Switch Build Configuration",
            placeHolder: "Configuration to activate (restarts the server)",
        });
        if (!chosen) {
            return;
        }
        let switched: SwitchConfigurationResult;
        try {
            switched = await client.sendRequest<SwitchConfigurationResult>(
                "clice/switchConfiguration",
                { name: chosen.label },
            );
        } catch {
            vscode.window.showWarningMessage(
                "clice: failed to switch build configuration — is the server running?",
            );
            return;
        }
        if (!switched.success) {
            vscode.window.showWarningMessage("clice: failed to switch build configuration");
            return;
        }
        if (chosen.label !== result.active) {
            // The choice is persisted only; the running server keeps its
            // configuration until it is started again.
            await vscode.commands.executeCommand("clice.restart");
        }
    }

    ext.subscriptions.push(
        status,
        vscode.commands.registerCommand("clice.switchConfiguration", select),
        // A (re)started server may run another configuration; a stopped
        // one has nothing to show (the request fails and hides the item).
        client.onDidChangeState((event) => {
            if (event.newState === State.Starting) {
                return;
            }
            void refresh();
        }),
    );
}
