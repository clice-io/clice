/// One-shot child processes for tests: `spawnSync`'s result without
/// blocking the event loop, which vitest's worker needs free to answer the
/// runner's RPC while a slow command runs.

import { spawn } from "node:child_process";

export interface ProcessOptions {
    cwd?: string;
    env?: NodeJS.ProcessEnv;
    /// Milliseconds until the child gets SIGTERM; no limit when unset.
    timeout?: number;
}

/// The fields of a `spawnSync(..., { encoding: "utf8" })` result; the
/// output is captured whole, with no buffer cap.
export interface ProcessResult {
    status: number | null;
    signal: NodeJS.Signals | null;
    stdout: string;
    stderr: string;
    /// Why the run failed: the spawn error, or the timeout.
    error?: Error;
}

/// Run `command` with an empty stdin until it exits. Never rejects: a
/// failed spawn or a timeout is reported in `error`.
export function runProcess(
    command: string,
    args: readonly string[],
    options: ProcessOptions = {},
): Promise<ProcessResult> {
    return new Promise((resolve) => {
        const child = spawn(command, args, { cwd: options.cwd, env: options.env });
        let stdout = "";
        let stderr = "";
        let error: Error | undefined;
        let timer: NodeJS.Timeout | undefined;
        let settled = false;
        const settle = (status: number | null, signal: NodeJS.Signals | null) => {
            if (settled) {
                return;
            }
            settled = true;
            clearTimeout(timer);
            resolve({ status, signal, stdout, stderr, ...(error && { error }) });
        };

        child.stdout.setEncoding("utf8").on("data", (chunk: string) => {
            stdout += chunk;
        });
        child.stderr.setEncoding("utf8").on("data", (chunk: string) => {
            stderr += chunk;
        });
        child.stdin.end();
        child.on("error", (spawnError) => {
            error = spawnError;
            settle(null, null);
        });
        child.on("close", settle);
        if (options.timeout !== undefined) {
            const timeout = options.timeout;
            timer = setTimeout(() => {
                error = new Error(`${command} timed out after ${timeout}ms`);
                child.kill("SIGTERM");
            }, timeout);
            // A grandchild still holding the pipes would delay `close` past
            // the child's death.
            child.on("exit", (status, signal) => {
                if (error) {
                    settle(status, signal);
                    child.stdout.destroy();
                    child.stderr.destroy();
                }
            });
        }
    });
}
