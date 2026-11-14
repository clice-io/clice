import * as path from "path";
import * as vscode from "vscode";
import { State } from "vscode-languageclient/node";
import type { ClientHandle } from "../client";
import type {
    LinkedBuild,
    ListConfigurationsResult,
    SwitchConfigurationResult,
} from "@clice/tools/protocol" with { "resolution-mode": "import" };

/// The directory of a linked build's first database, as the workspace
/// names it.
function buildDirectory(build: LinkedBuild): string {
    const database = vscode.Uri.parse(build.databases[0] ?? "");
    return vscode.workspace.asRelativePath(path.dirname(database.fsPath));
}

/** The build configuration switcher: a quick pick naming the configuration
 * the project of the active editor runs and the build its links resolve to,
 * which persists a new choice for that project, applied by the server asking
 * to be restarted. The editor's context menu offers it while the project's
 * rules declare configurations (`clice.configurations`). */
export function registerBuildConfiguration(client: ClientHandle, ext: vscode.ExtensionContext) {
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
        void vscode.commands.executeCommand(
            "setContext",
            "clice.configurations",
            (result?.configurations.length ?? 0) > 0,
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
            if (name === result.build?.configuration) {
                notes.push(`the links of ${result.active} resolve here`);
            }
            const linked =
                name === result.active && result.build
                    ? { detail: `follows its links to ${buildDirectory(result.build)}` }
                    : {};
            return { label: name, description: notes.join(", "), ...linked };
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
        vscode.commands.registerCommand("clice.switchConfiguration", select),
        // A (re)started server may run another configuration; a stopped
        // one has none (the request fails).
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
