/// Promise helpers shared by the test client and the replay runner.

/// The rejection of `withTimeout`: a caller that must tell a hang from any
/// other failure checks for it with `instanceof`.
export class TimeoutError extends Error {}

/// Settles with `promise`, or rejects with a TimeoutError once `ms`
/// elapsed first — `what` names the operation in the message. The timer is
/// cleared on either outcome of the promise, so a settled wait never keeps
/// the process alive; a rejection that is not an Error is wrapped in one.
export function withTimeout<T>(promise: Promise<T>, ms: number, what = ""): Promise<T> {
    return new Promise<T>((resolve, reject) => {
        const timer = setTimeout(() => {
            reject(new TimeoutError(`timed out after ${ms}ms${what ? `: ${what}` : ""}`));
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
