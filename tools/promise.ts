/// Promise helpers shared by the test client and the replay runner.

/// The rejection of `withTimeout`: a caller that must tell a hang from any
/// other failure checks for it with `instanceof`.
export class TimeoutError extends Error {}

/// Finishes with `promise`, or rejects with a TimeoutError once `ms`
/// elapsed first — `what` names the operation in the message. The timer is
/// cleared on either outcome of the promise, so a finished wait never keeps
/// the process alive; a rejection that is not an Error is wrapped in one.
export function withTimeout<T>(promise: Promise<T>, ms: number, what = ""): Promise<T> {
    return new Promise<T>((resolve, reject) => {
        // A process stalled past the deadline (a loaded machine) runs its due
        // timers before the I/O that became ready meanwhile: one more turn of
        // the event loop lets that I/O settle the promise first.
        let timer = setTimeout(() => {
            timer = setTimeout(() => {
                reject(new TimeoutError(`timed out after ${ms}ms${what ? `: ${what}` : ""}`));
            }, 0);
        }, ms);
        promise.then(
            (value) => {
                clearTimeout(timer);
                resolve(value);
            },
            (error: unknown) => {
                clearTimeout(timer);
                reject(error instanceof Error ? error : new Error(String(error)));
            },
        );
    });
}
