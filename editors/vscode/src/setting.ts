import * as vscode from "vscode";

interface Setting {
    executable: string | undefined;
    mode: string;
    host: string;
    port: number;
}

export function getSetting(): Setting | undefined {
    const setting = vscode.workspace.getConfiguration("clice");
    const executable = process.env.CLICE_EXECUTABLE || setting.get<string>("executable");
    const configuredMode = process.env.CLICE_MODE || setting.get<string>("mode");
    const mode = configuredMode === "socket" ? "tcp" : configuredMode;

    if (mode !== "pipe" && mode !== "tcp") {
        vscode.window.showErrorMessage(`Unexpected mode: ${mode}`);
        return undefined;
    }

    const host = setting.get<string>("host")!;
    const port = setting.get<number>("port")!;

    if (mode === "tcp" && (!host || !port)) {
        vscode.window.showErrorMessage("TCP mode requires both host and port to be configured.");
        return undefined;
    }

    return {
        executable,
        mode,
        host,
        port,
    };
}
