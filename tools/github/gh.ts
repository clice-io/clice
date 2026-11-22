/// The gh CLI as the PR and CI readers call it: one call at a time, a
/// second apart, a call GitHub answered with a server error tried again.
/// gh must be logged in, or GH_TOKEN set.

import { execFile } from "node:child_process";
import { setTimeout as sleep } from "node:timers/promises";
import { promisify } from "node:util";
import { REPO_ROOT } from "../repo.ts";

const exec = promisify(execFile);

/// What gh prints when GitHub, not the request, failed.
const TRANSIENT = /HTTP 5\d\d|timed? ?out|connection reset|unexpected EOF|TLS handshake/i;

/// The pauses before each retry of a call that failed transiently.
const RETRY_PAUSES_MS = [5_000, 15_000, 45_000];

/// gh's standard output for `args`. A failure prints gh's own message and
/// exits, as a reader can do nothing else with it.
export async function gh(args: readonly string[], retry = true): Promise<string> {
    const result = await call(args, retry);
    if (typeof result !== "string") {
        console.error(result.stderr.trim() || `gh ${args.join(" ")} failed`);
        process.exit(1);
    }
    return result;
}

/// gh's standard output for `args`, or its error output when GitHub refused
/// the request (a log that expired, a job that has none yet).
export async function ghTry(
    args: readonly string[],
): Promise<{ stdout: string } | { stderr: string }> {
    const result = await call(args);
    return typeof result === "string" ? { stdout: result } : result;
}

/// `retry` is off for a request that is not safe to repeat: GitHub may
/// have done it before it answered with the error.
async function call(args: readonly string[], retry = true): Promise<string | { stderr: string }> {
    for (let attempt = 0; ; attempt += 1) {
        try {
            const { stdout } = await exec("gh", args, {
                cwd: REPO_ROOT,
                maxBuffer: 512 * 1024 * 1024,
            });
            // Time is the feature: GitHub's secondary rate limit throttles
            // a client that calls in bursts; a second between calls stays
            // under it.
            await sleep(1_000);
            return stdout;
        } catch (error: unknown) {
            const stderr = (error as { stderr?: string }).stderr ?? String(error);
            const pause = RETRY_PAUSES_MS[attempt];
            if (!retry || pause === undefined || !TRANSIENT.test(stderr)) {
                return { stderr };
            }
            // No event: nothing announces that GitHub recovered from a
            // server error; the call is tried again after a pause.
            await sleep(pause);
        }
    }
}

export async function ghJson<T>(args: readonly string[], retry = true): Promise<T> {
    return JSON.parse(await gh(args, retry)) as T;
}

/// A GraphQL query's `data`; numbers go as typed variables, strings as
/// raw ones, and an undefined variable is left out.
export async function graphql<T>(
    query: string,
    variables: Readonly<Record<string, string | number | undefined>>,
    retry = true,
): Promise<T> {
    const args = ["api", "graphql", "-f", `query=${query}`];
    for (const [key, value] of Object.entries(variables)) {
        if (value !== undefined) {
            args.push(typeof value === "number" ? "-F" : "-f", `${key}=${String(value)}`);
        }
    }
    return (await ghJson<{ data: T }>(args, retry)).data;
}

let repository: { owner: string; name: string } | undefined;

/// The repository's owner and name.
export async function repo(): Promise<{ owner: string; name: string }> {
    if (repository === undefined) {
        const info = await ghJson<{ owner: { login: string }; name: string }>([
            "repo",
            "view",
            "--json",
            "owner,name",
        ]);
        repository = { owner: info.owner.login, name: info.name };
    }
    return repository;
}

/// The number of the current branch's PR.
export async function currentPr(): Promise<number> {
    return (await ghJson<{ number: number }>(["pr", "view", "--json", "number"])).number;
}

/// The author's login; GitHub answers a deleted account with no author.
export function login(node: { author?: { login: string } | null }): string {
    return node.author?.login ?? "ghost";
}
