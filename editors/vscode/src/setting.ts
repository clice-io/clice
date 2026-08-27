import * as vscode from "vscode";

export interface Setting {
    executable: string | undefined;
    mode: "pipe" | "socket";
    host: string;
    port: number;
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
        executable: executable === "" ? undefined : executable,
        mode,
        host,
        port,
    };
}
