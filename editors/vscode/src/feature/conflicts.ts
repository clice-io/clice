import * as vscode from "vscode";
import { State } from "vscode-languageclient/node";
import type { ClientHandle } from "../client";
import { workspaceTarget } from "../setting";

/// The setting turning an extension's language features off.
interface Switch {
    section: string;
    key: string;
    off: unknown;
    /// Whether `value` leaves them on.
    on: (value: unknown) => boolean;
}

/// Whether the features are on in a folder of the window, or in the window
/// when it has none: a folder's own value of a resource setting outranks the
/// workspace's.
function switchedOn(feature: Switch): boolean {
    const folders = vscode.workspace.workspaceFolders ?? [];
    const scopes = folders.length === 0 ? [undefined] : folders.map((folder) => folder.uri);
    return scopes.some((scope) =>
        feature.on(vscode.workspace.getConfiguration(feature.section, scope).get(feature.key)),
    );
}

/// Off in this workspace alone, its folders' own values included: the user's
/// settings and other workspaces' keep their choice.
async function switchOff(feature: Switch) {
    await vscode.workspace
        .getConfiguration(feature.section)
        .update(feature.key, feature.off, workspaceTarget());
    for (const folder of vscode.workspace.workspaceFolders ?? []) {
        const scoped = vscode.workspace.getConfiguration(feature.section, folder.uri);
        if (feature.on(scoped.get(feature.key))) {
            await scoped.update(
                feature.key,
                feature.off,
                vscode.ConfigurationTarget.WorkspaceFolder,
            );
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
    switch?: Switch;
}

const known: ConflictingExtension[] = [
    {
        id: "ms-vscode.cpptools",
        name: "C/C++ (cpptools)",
        switch: {
            section: "C_Cpp",
            key: "intelliSenseEngine",
            off: "disabled",
            on: (value) => typeof value !== "string" || value.toLowerCase() !== "disabled",
        },
    },
    {
        id: "llvm-vs-code-extensions.vscode-clangd",
        name: "clangd",
        switch: { section: "clangd", key: "enable", off: false, on: (value) => value !== false },
    },
    {
        id: "ccls-project.ccls",
        name: "ccls",
    },
];

const ignoreKey = "ignoreConflictingExtensions";

/// Other C/C++ language extensions running next to clice produce duplicate
/// completion, hover and diagnostics. Detect the known ones and offer to
/// turn their language features off in this workspace, the way clangd
/// handles cpptools. vscode.extensions only surfaces enabled extensions, so
/// a hit means installed and enabled; there is no API to disable an
/// extension, hence the settings-level switches.
export function registerConflictCheck(client: ClientHandle, ext: vscode.ExtensionContext) {
    /// The extensions warned about in this window, while they conflict: a
    /// restart of clice does not repeat a warning the user dismissed, and one
    /// that stops conflicting and conflicts again is warned about anew.
    const warned = new Set<string>();

    async function check() {
        // Only nag while clice itself is up: disabling the working provider
        // when clice cannot start would leave no language service at all.
        if (!client.isRunning() || ext.globalState.get<boolean>(ignoreKey)) {
            return;
        }
        const found = known.filter(
            (candidate) =>
                vscode.extensions.getExtension(candidate.id) &&
                (!candidate.switch || switchedOn(candidate.switch)),
        );
        for (const id of warned) {
            if (!found.some((candidate) => candidate.id === id)) {
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
        const disable =
            workspaceTarget() === vscode.ConfigurationTarget.Workspace
                ? "Disable in Workspace"
                : "Disable in User Settings";
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
            if (candidate.switch) {
                await switchOff(candidate.switch);
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
