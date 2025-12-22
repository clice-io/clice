import * as vscode from 'vscode';

interface Setting {
    executable: string | undefined,
    mode: string,
    host: string,
    port: number,
}

export function getSetting(): Setting | undefined {
    const setting = vscode.workspace.getConfiguration('clice')
    const executable = setting.get<string>('executable');
    const mode = setting.get<string>('mode');

    if (mode !== "pipe" && mode !== "socket") {
        vscode.window.showErrorMessage(`Unexpected mode: ${mode}`);
        return undefined
    }

    const host = setting.get<string>('host')!;
    const port = setting.get<number>('port')!;

    if (mode === "socket" && (!host || !port)) {
        vscode.window.showErrorMessage('Socket mode requires both host and port to be configured.');
        return undefined;
    }

    return {
        executable, mode, host, port,
    }
}
