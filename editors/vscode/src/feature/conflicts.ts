import * as vscode from "vscode";
import { State } from "vscode-languageclient/node";
import type { ClientHandle } from "../client";

/// Write to user scope and clear any workspace or folder override:
/// those scopes outrank global settings, so a repository shipping the
/// setting in .vscode/settings.json would silently win otherwise.
async function overrideSetting(section: string, key: string, value: unknown) {
    const config = vscode.workspace.getConfiguration(section);
    await config.update(key, value, vscode.ConfigurationTarget.Global);
    if (config.inspect(key)?.workspaceValue !== undefined) {
        await config.update(key, undefined, vscode.ConfigurationTarget.Workspace);
    }
    for (const folder of vscode.workspace.workspaceFolders ?? []) {
        const scoped = vscode.workspace.getConfiguration(section, folder.uri);
        if (scoped.inspect(key)?.workspaceFolderValue !== undefined) {
            await scoped.update(key, undefined, vscode.ConfigurationTarget.WorkspaceFolder);
        }
    }
}

interface ConflictingExtension {
    id: string;
    name: string;
    /// Whether the installed extension currently has its language features
    /// on; an installed-but-neutered one (cpptools with IntelliSense
    /// disabled, kept for its debugger) coexists fine.
    conflicts: () => boolean;
    /// Turn the extension's language features off, or undefined when it
    /// has no such setting and must be disabled from the Extensions view.
    disable?: () => Thenable<unknown>;
}

const known: ConflictingExtension[] = [
    {
        id: "ms-vscode.cpptools",
        name: "C/C++ (cpptools)",
        // intelliSenseEngine is resource-scoped, so what cpptools serves
        // here is the per-folder effective value; the unscoped value only
        // matters when no folder is open.
        conflicts: () => {
            const enabled = (config: vscode.WorkspaceConfiguration) =>
                config.get<string>("intelliSenseEngine", "default").toLowerCase() !== "disabled";
            const folders = vscode.workspace.workspaceFolders ?? [];
            if (folders.length === 0) {
                return enabled(vscode.workspace.getConfiguration("C_Cpp"));
            }
            return folders.some((folder) =>
                enabled(vscode.workspace.getConfiguration("C_Cpp", folder.uri)),
            );
        },
        disable: () => overrideSetting("C_Cpp", "intelliSenseEngine", "disabled"),
    },
    {
        id: "llvm-vs-code-extensions.vscode-clangd",
        name: "clangd",
        conflicts: () => vscode.workspace.getConfiguration("clangd").get<boolean>("enable", true),
        disable: () => overrideSetting("clangd", "enable", false),
    },
    {
        id: "ccls-project.ccls",
        name: "ccls",
        conflicts: () => true,
    },
];

const ignoreKey = "ignoreConflictingExtensions";

/// Other C/C++ language extensions running next to clice produce duplicate
/// completion, hover and diagnostics. Detect the known ones and offer to
/// turn their language features off, the way clangd handles cpptools.
/// vscode.extensions only surfaces enabled extensions, so a hit means
/// installed and enabled; there is no API to disable an extension, hence
/// the settings-level switches.
export function registerConflictCheck(client: ClientHandle, ext: vscode.ExtensionContext) {
    let prompting = false;

    async function check() {
        // Only nag while clice itself is up: disabling the working provider
        // when clice cannot start would leave no language service at all.
        if (prompting || !client.isRunning() || ext.globalState.get<boolean>(ignoreKey)) {
            return;
        }
        const found = known.filter(
            (candidate) => vscode.extensions.getExtension(candidate.id) && candidate.conflicts(),
        );
        if (found.length === 0) {
            return;
        }
        prompting = true;
        try {
            const names = found.map((candidate) => candidate.name).join(", ");
            const choice = await vscode.window.showWarningMessage(
                `clice provides the C/C++ language features, and running ${names} alongside ` +
                    "it duplicates completion and diagnostics. Disable the conflicting " +
                    "language features?",
                "Disable",
                "Never ask again",
            );
            if (choice === "Never ask again") {
                await ext.globalState.update(ignoreKey, true);
                return;
            }
            if (choice !== "Disable") {
                return;
            }
            let disabled = false;
            for (const candidate of found) {
                if (candidate.disable) {
                    await candidate.disable();
                    disabled = true;
                } else {
                    await vscode.commands.executeCommand(
                        "workbench.extensions.search",
                        `@installed ${candidate.name}`,
                    );
                    void vscode.window.showInformationMessage(
                        `clice: ${candidate.name} has no setting to turn its language ` +
                            "features off — disable the extension itself from the opened " +
                            "Extensions view.",
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
        } finally {
            prompting = false;
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
