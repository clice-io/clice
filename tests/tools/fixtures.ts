/// vitest fixtures: the `session` factory spawns clice servers bound to
/// test-data workspaces, with the teardown gates every test must pass
/// (clean shutdown, no anomalies).

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test as base } from "vitest";
import { assertNoAnomaly } from "./checks.ts";
import { CliceClient } from "./client.ts";
import { DATA_DIR, generateCdb } from "./compile_commands.ts";
import { shutdownClient } from "./lifecycle.ts";

export { expect } from "vitest";

export function cliceExecutable(): string {
    let exe = process.env["CLICE_EXECUTABLE"];
    if (!exe) {
        throw new Error("CLICE_EXECUTABLE is not set; point it at build/<type>/bin/clice");
    }
    if (process.platform === "win32" && !exe.toLowerCase().endsWith(".exe")) {
        const withSuffix = `${exe}.exe`;
        if (fs.existsSync(withSuffix) || !fs.existsSync(exe)) {
            exe = withSuffix;
        }
    }
    if (!fs.existsSync(exe)) {
        throw new Error(`clice executable not found at '${exe}'`);
    }
    return path.resolve(exe);
}

// Tests sharing a data workspace mutate it (.clice cleanup, cmake
// regeneration). Files run in parallel workers, so exclusivity is a
// cross-process lock, not scheduler grouping. Locks live in tmpdir; a
// crashed run's leftover lock (owner pid no longer alive) is stolen.
const LOCKS_DIR = path.join(os.tmpdir(), "clice-test-workspace-locks");

function sleep(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

function pidAlive(pid: number): boolean {
    try {
        process.kill(pid, 0);
        return true;
    } catch {
        return false;
    }
}

async function acquireWorkspaceLock(name: string): Promise<() => void> {
    fs.mkdirSync(LOCKS_DIR, { recursive: true });
    const lock = path.join(LOCKS_DIR, name.replaceAll(/[\\/]/g, "__"));
    const pidFile = path.join(lock, "pid");
    for (;;) {
        try {
            fs.mkdirSync(lock);
        } catch {
            // Held. Steal only when the recorded owner is dead (a crashed run's
            // leftover); a live owner is never stolen, no matter how old.
            let ownerDead = false;
            try {
                ownerDead = !pidAlive(Number(fs.readFileSync(pidFile, "utf8")));
            } catch {
                // No pid file yet: the owner is between mkdir and write — alive.
            }
            if (ownerDead) {
                fs.rmSync(lock, { recursive: true, force: true });
            } else {
                await sleep(100);
            }
            continue;
        }
        fs.writeFileSync(pidFile, String(process.pid));
        // Dead-owner stealing has a window: a rival that read the dead pid
        // right before we recreated the lock may still remove it under us.
        // Confirm ownership after the write settles; losers just retry.
        await sleep(50);
        try {
            if (Number(fs.readFileSync(pidFile, "utf8")) === process.pid) {
                return () => {
                    fs.rmSync(lock, { recursive: true, force: true });
                };
            }
        } catch {
            // Our lock was removed — lost the race.
        }
    }
}

export interface Session {
    client: CliceClient;
    workspace: string;
}

export interface SessionOptions {
    initializationOptions?: Record<string, unknown> | undefined;
    /// Anomalies are internal clice bugs — every test session must end
    /// without one. Tests that intentionally trigger anomalies opt out here
    /// and assert on them explicitly.
    allowAnomaly?: boolean | undefined;
    drainStderr?: boolean | undefined;
}

export interface SessionFactory {
    /// Spawn a server initialized on tests/data/<name>. Acquire sessions for
    /// multiple workspaces in alphabetical order to avoid lock cycles.
    (name: string, options?: SessionOptions): Promise<Session>;
    /// Spawn a bare server with no workspace/initialize.
    bare(options?: SessionOptions & { args?: string[] | undefined }): CliceClient;
    /// Spawn a server bound to a fresh, empty temp workspace, without
    /// initializing. The caller writes fixture files (and a
    /// compile_commands.json) then calls client.initialize(workspace). The
    /// whole temp directory is removed in teardown.
    tmp(options?: SessionOptions): Session;
}

interface OpenedSession {
    client: CliceClient;
    workspace: string | null;
    allowAnomaly: boolean;
    /// Temp workspaces are removed whole; static data workspaces keep only
    /// their .clice/ dropped.
    isTemp: boolean;
}

function prepareWorkspace(workspace: string): void {
    if (fs.existsSync(path.join(workspace, "CMakeLists.txt"))) {
        generateCdb(workspace);
    }
    // Clean up persisted index/cache so each test starts fresh.
    fs.rmSync(path.join(workspace, ".clice"), { recursive: true, force: true });
}

/// The zero-boilerplate form for files whose tests all target one
/// workspace — the TS equivalent of pytest's `@pytest.mark.workspace`
/// marker plus `client`/`workspace` fixtures:
///
///     const test = cliceTest("document_links");
///     test("links with pch", async ({ client, workspace }) => { ... });
///
/// Setup (spawn + initialize) and teardown (shutdown gate, anomaly gate,
/// .clice cleanup, workspace lock) are fully automatic. Tests needing
/// several servers, custom argv or a temp workspace use the `session`
/// factory from `test` instead.
export function cliceTest(name: string, options: SessionOptions = {}) {
    return test.extend<Session & { bound: Session }>({
        bound: async (
            { session }: { session: SessionFactory },
            use: (bound: Session) => Promise<void>,
        ) => {
            await use(await session(name, options));
        },
        client: async (
            { bound }: { bound: Session },
            use: (client: CliceClient) => Promise<void>,
        ) => {
            await use(bound.client);
        },
        workspace: async (
            { bound }: { bound: Session },
            use: (workspace: string) => Promise<void>,
        ) => {
            await use(bound.workspace);
        },
    });
}

export const test = base.extend<{ session: SessionFactory }>({
    session: async (
        { task }: { task: { result?: { errors?: unknown[] } } },
        use: (factory: SessionFactory) => Promise<void>,
    ) => {
        const opened: OpenedSession[] = [];
        const releases: (() => void)[] = [];

        const factory = async (name: string, options: SessionOptions = {}): Promise<Session> => {
            const workspace = path.join(DATA_DIR, name);
            releases.push(await acquireWorkspaceLock(name));
            prepareWorkspace(workspace);
            const client = CliceClient.start(cliceExecutable(), {
                drainStderr: options.drainStderr,
            });
            opened.push({
                client,
                workspace,
                allowAnomaly: options.allowAnomaly ?? false,
                isTemp: false,
            });
            await client.initialize(workspace, {
                initializationOptions: options.initializationOptions,
            });
            return { client, workspace };
        };
        factory.bare = (
            options: SessionOptions & { args?: string[] | undefined } = {},
        ): CliceClient => {
            const client = CliceClient.start(cliceExecutable(), {
                drainStderr: options.drainStderr,
                args: options.args,
            });
            opened.push({
                client,
                workspace: null,
                allowAnomaly: options.allowAnomaly ?? false,
                isTemp: false,
            });
            return client;
        };
        factory.tmp = (options: SessionOptions = {}): Session => {
            const workspace = fs.mkdtempSync(path.join(os.tmpdir(), "clice-test-"));
            const client = CliceClient.start(cliceExecutable(), {
                drainStderr: options.drainStderr,
            });
            opened.push({
                client,
                workspace,
                allowAnomaly: options.allowAnomaly ?? false,
                isTemp: true,
            });
            return { client, workspace };
        };

        try {
            await use(factory);
        } finally {
            const failed = (task.result?.errors?.length ?? 0) > 0;
            const teardownErrors: unknown[] = [];
            for (const session of opened.reverse()) {
                // The anomaly gate must run even when shutdown itself fails — a
                // crashed server is exactly when the anomaly evidence matters most.
                try {
                    await shutdownClient(session.client, { verbose: failed });
                } catch (exc) {
                    teardownErrors.push(exc);
                } finally {
                    try {
                        if (!session.allowAnomaly) {
                            assertNoAnomaly(session.client, session.workspace);
                        }
                    } catch (exc) {
                        teardownErrors.push(exc);
                    }
                    if (session.workspace !== null) {
                        fs.rmSync(
                            session.isTemp
                                ? session.workspace
                                : path.join(session.workspace, ".clice"),
                            { recursive: true, force: true },
                        );
                    }
                }
            }
            releases.reverse().forEach((release) => {
                release();
            });
            if (teardownErrors.length > 0) {
                throw teardownErrors[0];
            }
        }
    },
});
