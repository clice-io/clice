/// One-shot child processes for tests: `spawnSync`'s result without
/// blocking the event loop, which vitest's worker needs free to answer the
/// runner's RPC while a slow command runs.

import { spawn } from "node:child_process";

export interface ProcessOptions {
    cwd?: string;
    env?: NodeJS.ProcessEnv;
    /// Milliseconds until the child gets SIGTERM; no limit when unset.
    timeout?: number;
    /// What the child reads on stdin; nothing when unset.
    input?: string;
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

/// Run `command` until it exits, its stdin holding `options.input`. Never
/// rejects: a failed spawn or a timeout is reported in `error`.
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
        let finished = false;
        const finish = (status: number | null, signal: NodeJS.Signals | null) => {
            if (finished) {
                return;
            }
            finished = true;
            clearTimeout(timer);
            resolve({ status, signal, stdout, stderr, ...(error && { error }) });
        };

        child.stdout.setEncoding("utf8").on("data", (chunk: string) => {
            stdout += chunk;
        });
        child.stderr.setEncoding("utf8").on("data", (chunk: string) => {
            stderr += chunk;
        });
        // A child exiting before it reads its input breaks the pipe; its
        // exit, not the failed write, reports the run.
        child.stdin.on("error", () => undefined);
        child.stdin.end(options.input);
        child.on("error", (spawnError) => {
            error = spawnError;
            finish(null, null);
        });
        child.on("close", finish);
        if (options.timeout !== undefined) {
            const timeout = options.timeout;
            // Hang detection: the caller's bound on a run that never ends.
            timer = setTimeout(() => {
                error = new Error(`${command} timed out after ${timeout}ms`);
                child.kill("SIGTERM");
            }, timeout);
            // A grandchild still holding the pipes would delay `close` past
            // the child's death.
            child.on("exit", (status, signal) => {
                if (error) {
                    finish(status, signal);
                    child.stdout.destroy();
                    child.stderr.destroy();
                }
            });
        }
    });
}

/// Whether a process has the pid: signal 0 checks without sending, and
/// ESRCH says none has.
export function running(pid: number): boolean {
    try {
        process.kill(pid, 0);
        return true;
    } catch (error) {
        if ((error as NodeJS.ErrnoException).code === "ESRCH") {
            return false;
        }
        throw error;
    }
}
