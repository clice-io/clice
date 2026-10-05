import * as fs from "fs";
import * as path from "path";
import * as vscode from "vscode";

export interface Setting {
    executable: string | undefined;
    mode: "pipe" | "socket";
    host: string;
    port: number;
}

/// The directory the server starts in and a relative clice.executable
/// resolves against: the first workspace folder, not wherever VS Code
/// happened to be started from — when it exists (a .code-workspace may
/// list a folder missing on this machine).
export function workspaceDirectory(): string | undefined {
    const folder = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
    return folder && fs.existsSync(folder) ? folder : undefined;
}

/// A path spelled relative to the workspace resolves against `base`; a
/// bare command name stays for the PATH lookup.
export function resolveExecutable(executable: string, base: string | undefined): string {
    const relative = !path.isAbsolute(executable) && /[\\/]/.test(executable);
    return relative && base ? path.resolve(base, executable) : executable;
}

/// Read every launch-relevant setting fresh. Throws on invalid values; the
/// caller surfaces the message before the server is (re)started.
export function getSetting(): Setting {
    const setting = vscode.workspace.getConfiguration("clice");
    const executable = process.env.CLICE_EXECUTABLE ?? setting.get<string>("executable");
    const mode = process.env.CLICE_MODE ?? setting.get<string>("mode");

    if (mode !== "pipe" && mode !== "socket") {
        throw new Error(`unexpected clice.mode: ${mode ?? "(unset)"}`);
    }

    const host = setting.get<string>("host") ?? "";
    const port = setting.get<number>("port") ?? 0;

    if (mode === "socket" && (!host || !Number.isInteger(port) || port < 1 || port > 65535)) {
        throw new Error("socket mode requires clice.host and a clice.port between 1 and 65535");
    }

    return {
        executable: executable ? resolveExecutable(executable, workspaceDirectory()) : undefined,
        mode,
        host,
        port,
    };
}
