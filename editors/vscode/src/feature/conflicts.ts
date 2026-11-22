import * as vscode from "vscode";
import { State } from "vscode-languageclient/node";
import type { ClientHandle } from "../client";
import { enabled, workspaceTarget } from "../setting";

/// The setting turning an extension's language features off.
interface Toggle {
    section: string;
    key: string;
    off: unknown;
    enables: (value: unknown) => boolean;
}

/// Whether the features are on in a folder of the window, or in the window
/// when it has none: a folder's own value of a resource setting outranks the
/// workspace's.
function featuresOn(toggle: Toggle): boolean {
    const folders = vscode.workspace.workspaceFolders ?? [];
    const scopes = folders.length === 0 ? [undefined] : folders.map((folder) => folder.uri);
    return scopes.some((scope) =>
        toggle.enables(vscode.workspace.getConfiguration(toggle.section, scope).get(toggle.key)),
    );
}

/// Off in this workspace alone, its folders' own values included: the user's
/// settings and other workspaces' keep their choice.
async function turnOff(toggle: Toggle) {
    await vscode.workspace
        .getConfiguration(toggle.section)
        .update(toggle.key, toggle.off, workspaceTarget());
    for (const folder of vscode.workspace.workspaceFolders ?? []) {
        const scoped = vscode.workspace.getConfiguration(toggle.section, folder.uri);
        if (toggle.enables(scoped.get(toggle.key))) {
            await scoped.update(toggle.key, toggle.off, vscode.ConfigurationTarget.WorkspaceFolder);
        }
    }
}

interface ConflictingExtension {
    id: string;
    name: string;
    /// Undefined when the extension has no such setting and must be disabled
    /// from the Extensions view. An installed one with its language features
    /// off (cpptools with IntelliSense disabled, kept for its debugger)
    /// coexists fine.
    toggle?: Toggle;
}

const known: ConflictingExtension[] = [
    {
        id: "ms-vscode.cpptools",
        name: "C/C++ (cpptools)",
        toggle: {
            section: "C_Cpp",
            key: "intelliSenseEngine",
            off: "disabled",
            enables: (value) => typeof value !== "string" || value.toLowerCase() !== "disabled",
        },
    },
    {
        id: "llvm-vs-code-extensions.vscode-clangd",
        name: "clangd",
        toggle: {
            section: "clangd",
            key: "enable",
            off: false,
            enables: (value) => value !== false,
        },
    },
    {
        id: "ccls-project.ccls",
        name: "ccls",
    },
];

const ignoreKey = "ignoreConflictingExtensions";

/// Other C/C++ language extensions running next to clice produce duplicate
/// completion, hover and diagnostics. Detect the known ones and offer to
/// turn their language features off in this workspace. vscode.extensions
/// only surfaces enabled extensions, so a hit means installed and enabled;
/// there is no API to disable an extension, hence the settings that turn
/// their language features off.
export function registerConflictCheck(client: ClientHandle, ext: vscode.ExtensionContext) {
    /// The extensions warned about in this window. A warning the user
    /// dismissed is not repeated, as clice restarts or as the extension's
    /// setting turns its features off and on again; one installed or enabled
    /// again once gone is news.
    const warned = new Set<string>();

    async function check() {
        // Only nag while clice itself is up, and stays up: disabling the
        // working provider when clice cannot start, or was turned off while
        // starting, would leave no language service at all.
        if (!client.isRunning() || !enabled() || ext.globalState.get<boolean>(ignoreKey)) {
            return;
        }
        const found = known.filter(
            (candidate) =>
                vscode.extensions.getExtension(candidate.id) &&
                (!candidate.toggle || featuresOn(candidate.toggle)),
        );
        for (const id of warned) {
            if (!vscode.extensions.getExtension(id)) {
                warned.delete(id);
            }
        }
        const fresh = found.filter((candidate) => !warned.has(candidate.id));
        if (fresh.length === 0) {
            return;
        }
        for (const candidate of fresh) {
            warned.add(candidate.id);
        }
        const names = fresh.map((candidate) => candidate.name).join(", ");
        // A window without a workspace has only the user's settings, which
        // turn them off in every workspace.
        const inWorkspace = workspaceTarget() === vscode.ConfigurationTarget.Workspace;
        const disable = inWorkspace ? "Disable in Workspace" : "Disable in User Settings";
        const choice = await vscode.window.showWarningMessage(
            `clice provides the C/C++ language features, and running ${names} alongside ` +
                "it duplicates completion and diagnostics. Disable the conflicting " +
                "language features?",
            disable,
            "Never ask again",
        );
        if (choice === "Never ask again") {
            await ext.globalState.update(ignoreKey, true);
            return;
        }
        if (choice !== disable) {
            return;
        }
        let disabled = false;
        for (const candidate of fresh) {
            if (candidate.toggle) {
                await turnOff(candidate.toggle);
                disabled = true;
            } else {
                await vscode.commands.executeCommand(
                    "workbench.extensions.search",
                    `@installed ${candidate.name}`,
                );
                // The view's Disable disables it in every workspace.
                void vscode.window.showInformationMessage(
                    `clice: ${candidate.name} has no setting to turn its language ` +
                        "features off — disable the extension itself from the opened " +
                        (inWorkspace
                            ? "Extensions view, with Disable (Workspace)."
                            : "Extensions view."),
                );
            }
        }
        if (disabled) {
            // A conflicting server that already activated keeps serving
            // until the window reloads; the settings only stop the next
            // activation.
            const reload = await vscode.window.showInformationMessage(
                "clice: conflicting language features disabled — reload the window to " +
                    "fully apply.",
                "Reload Window",
            );
            if (reload === "Reload Window") {
                await vscode.commands.executeCommand("workbench.action.reloadWindow");
            }
        }
    }

    ext.subscriptions.push(
        vscode.extensions.onDidChange(() => {
            void check();
        }),
        client.onDidChangeState((event) => {
            if (event.newState === State.Running) {
                void check();
            }
        }),
    );
}
