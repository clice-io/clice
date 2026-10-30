import * as vscode from "vscode";
import { State } from "vscode-languageclient/node";
import type { ClientHandle } from "../client";
import { LanguageStatus } from "./status";
import type {
    ListConfigurationsResult,
    SwitchConfigurationResult,
} from "@clice/tools/protocol" with { "resolution-mode": "import" };

/** The build configuration switcher: a language status item naming the
 * configuration the project of the active editor runs (only while its
 * rules declare any) and a quick pick that persists a new choice for that
 * project and restarts the server, since a selection takes effect at the
 * next start. */
export function registerBuildConfiguration(client: ClientHandle, ext: vscode.ExtensionContext) {
    const status = new LanguageStatus("clice.configuration", "clice: build configuration");
    const switchConfiguration: vscode.Command = {
        title: "Switch",
        command: "clice.switchConfiguration",
    };

    /// The document naming the project; without one the server answers
    /// for its first folder.
    function project(): { uri?: string } {
        const document = vscode.window.activeTextEditor?.document;
        return document?.uri.scheme === "file" ? { uri: document.uri.toString() } : {};
    }

    async function list(
        target: { uri?: string } = project(),
    ): Promise<ListConfigurationsResult | undefined> {
        try {
            return await client.sendRequest<ListConfigurationsResult>(
                "clice/listConfigurations",
                target,
            );
        } catch {
            return undefined;
        }
    }

    /// Bumped by every refresh; a response is dropped when a newer request
    /// started while it was in flight.
    let generation = 0;

    async function refresh() {
        generation += 1;
        const current = generation;
        const result = await list();
        if (current !== generation) {
            return;
        }
        if (!result || result.configurations.length === 0) {
            status.hide();
            return;
        }
        status.show(
            result.active,
            result.selected && result.selected !== result.active
                ? `the build configuration; ${result.selected} is selected for the next start`
                : "the build configuration",
            switchConfiguration,
        );
    }

    async function select() {
        // The editor may change while the pick is open; the choice belongs
        // to the project the menu came from.
        const target = project();
        const result = await list(target);
        if (!result) {
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
            if (name === result.selected && name !== result.active) {
                notes.push("selected for the next start");
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
                { name: chosen.label, ...target },
            );
        } catch {
            vscode.window.showWarningMessage(
                "clice: failed to switch build configuration — is the server running?",
            );
            return;
        }
        if (!switched.success) {
            vscode.window.showWarningMessage(
                `clice: cannot switch the build configuration: ${switched.reason}`,
            );
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
        // Each project has its own configurations.
        vscode.window.onDidChangeActiveTextEditor(() => {
            if (client.isRunning()) {
                void refresh();
            }
        }),
    );
}
