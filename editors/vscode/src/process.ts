import type * as cp from "child_process";

/// Signal deaths leave exitCode null and set signalCode only.
export function exited(proc: cp.ChildProcess): boolean {
    return proc.exitCode !== null || proc.signalCode !== null;
}

/// Resolves once `proc` has exited.
export function exitOf(proc: cp.ChildProcess): Promise<void> {
    return new Promise((resolve) => {
        if (exited(proc)) {
            resolve();
            return;
        }
        proc.once("exit", () => {
            resolve();
        });
    });
}

/// Whether `proc` exits within `ms`.
export async function exitsWithin(proc: cp.ChildProcess, ms: number): Promise<boolean> {
    let timer: NodeJS.Timeout | undefined;
    const timeout = new Promise<boolean>((resolve) => {
        timer = setTimeout(() => {
            resolve(false);
        }, ms);
    });
    const done = await Promise.race([exitOf(proc).then(() => true), timeout]);
    clearTimeout(timer);
    return done;
}

export interface RetireTimings {
    /// How long a server told to exit gets before it is signalled.
    exit: number;
    /// How long SIGTERM, which saves and exits as LSP `exit` does, gets
    /// before SIGKILL. Windows has no polite signal: the wait runs on.
    term: number;
}

/// Resolves once `previous` has exited. A server still saving its index
/// holds the workspace's index lock, and one started beside it would run
/// without the index.
export async function retire(
    previous: cp.ChildProcess,
    timings: RetireTimings,
    log: (line: string) => void,
): Promise<void> {
    if (await exitsWithin(previous, timings.exit)) {
        return;
    }
    if (process.platform !== "win32") {
        log(`clice server ${previous.pid} has not exited; sending SIGTERM`);
        previous.kill("SIGTERM");
    }
    if (await exitsWithin(previous, timings.term)) {
        return;
    }
    log(`clice server ${previous.pid} has not exited; killing it`);
    previous.kill("SIGKILL");
    await exitOf(previous);
}
