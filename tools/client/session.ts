/// Session machinery for the test suites: executable resolution, cross-
/// process workspace locks, and the resource-managing session factory
/// with its teardown gates (clean shutdown, no anomalies). Framework-
/// agnostic — each suite binds it to vitest in its own thin fixture file
/// (tests/integration/fixtures.ts, tests/snap/fixtures.ts).

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { CliceClient, type InitializeOptions, type StartOptions } from "./client.ts";
import { Workspace } from "./workspace.ts";
import { DATA_DIR, generateCDB } from "../compile_commands.ts";

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
    const startedAt = Date.now();
    let warned = false;

    // The mkdir lock alone is unfair: a worker whose tests run back-to-back
    // releases and re-acquires within microseconds, while cross-process
    // waiters poll on a 100ms clock and lose that window essentially every
    // time (observed as 120s starvation of a single test in CI). A FIFO
    // ticket queue in front of the mutex restores fairness: only the oldest
    // live ticket may take the lock. Tickets of dead processes are removed
    // by whoever notices them.
    const queue = `${lock}.queue`;
    fs.mkdirSync(queue, { recursive: true });
    const ticketName = `${String(Date.now()).padStart(15, "0")}-${String(process.pid).padStart(8, "0")}`;
    const ticket = path.join(queue, ticketName);
    fs.writeFileSync(ticket, String(process.pid));
    const dropTicket = () => {
        fs.rmSync(ticket, { force: true });
    };

    const isMyTurn = (): boolean => {
        let entries: string[];
        try {
            entries = fs.readdirSync(queue).sort();
        } catch {
            return true;
        }
        for (const entry of entries) {
            if (entry === ticketName) {
                return true;
            }
            let owner = Number.NaN;
            try {
                owner = Number(fs.readFileSync(path.join(queue, entry), "utf8"));
            } catch {
                continue; // Ticket vanished between readdir and read.
            }
            if (pidAlive(owner)) {
                return false; // A live earlier ticket goes first.
            }
            fs.rmSync(path.join(queue, entry), { force: true });
        }
        return true;
    };

    for (;;) {
        // A silent multi-minute wait here looks like a hung suite; surface
        // contention early and fail loudly instead of starving forever.
        const waited = Date.now() - startedAt;
        if (!warned && waited > 5_000) {
            warned = true;
            console.warn(`[session] waiting on workspace lock ${name} (${waited}ms)`);
        }
        if (waited > 240_000) {
            let holder = "unknown";
            try {
                holder = fs.readFileSync(pidFile, "utf8");
            } catch {
                // Holder mid-transition; report what we have.
            }
            dropTicket();
            throw new Error(
                `workspace lock ${name} starved for ${waited}ms (holder pid ${holder})`,
            );
        }
        if (!isMyTurn()) {
            await sleep(100);
            continue;
        }
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
                    dropTicket();
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
    workspace: Workspace;
}

export interface SessionOptions extends InitializeOptions, StartOptions {
    /// Anomalies are internal clice bugs — every test session must end
    /// without one. Tests that intentionally trigger anomalies opt out here
    /// and assert on them explicitly.
    allowAnomaly?: boolean | undefined;
    /// When set, spawn in `--mode socket` and connect the LSP transport over
    /// this TCP port instead of stdio (args must request socket mode).
    socketPort?: number | undefined;
}

/// The session factory doubles as the test's resource manager — the
/// fixture-teardown equivalent of a destructor. Every server it spawns
/// and every temp workspace it mints is registered and reclaimed
/// automatically (shutdown gate, anomaly gate, removal), so tests never
/// write try/finally cleanup. A client already shut down explicitly via
/// client.shutdown() (restart tests) is skipped by the teardown.
export interface SessionFactory {
    /// Spawn a server initialized on tests/data/<name>. Acquire sessions
    /// for multiple workspaces in alphabetical order to avoid lock cycles.
    (name: string, options?: SessionOptions): Promise<Session>;
    /// Spawn a server bound to a fresh, empty temp workspace, without
    /// initializing. The caller writes fixture files (and a CDB) then calls
    /// client.initialize(workspace). The whole temp directory is removed in
    /// teardown.
    tmp(options?: SessionOptions): Session;
    /// A fresh temp workspace with no server, removed in teardown.
    tmpdir(): Workspace;
    /// Spawn a tracked server against an existing workspace (e.g. a second
    /// session over a tmpdir() in restart tests), without initializing. The
    /// workspace's lifetime is not affected.
    spawn(workspace: Workspace | null, options?: SessionOptions): CliceClient;
}

/// A live factory plus its teardown. The suite fixture calls teardown once
/// the test is over, passing whether the test failed (controls shutdown
/// verbosity); the first teardown-gate error is rethrown.
export interface SessionHandle {
    session: SessionFactory;
    teardown(failed: boolean): Promise<void>;
}

interface OpenedSession {
    client: CliceClient;
    workspace: Workspace | null;
    allowAnomaly: boolean;
}

function prepareWorkspace(workspace: Workspace): void {
    if (workspace.exists("CMakeLists.txt")) {
        generateCDB(workspace.root);
    }
    // Clean up persisted index/cache so each test starts fresh.
    workspace.rm(".clice");
}

export function createSessionFactory(): SessionHandle {
    const opened: OpenedSession[] = [];
    const tempDirs: Workspace[] = [];
    const releases: (() => void)[] = [];

    const spawnTracked = (
        workspace: Workspace | null,
        options: SessionOptions = {},
    ): CliceClient => {
        const client = CliceClient.start(cliceExecutable(), {
            drainStderr: options.drainStderr,
            args: options.args,
        });
        opened.push({
            client,
            workspace,
            allowAnomaly: options.allowAnomaly ?? false,
        });
        return client;
    };

    const factory = async (name: string, options: SessionOptions = {}): Promise<Session> => {
        const workspace = new Workspace(path.join(DATA_DIR, name));
        releases.push(await acquireWorkspaceLock(name));
        prepareWorkspace(workspace);
        const client =
            options.socketPort !== undefined
                ? await CliceClient.startSocket(cliceExecutable(), options.socketPort, {
                      args: options.args,
                  })
                : CliceClient.start(cliceExecutable(), {
                      drainStderr: options.drainStderr,
                      args: options.args,
                  });
        opened.push({
            client,
            workspace,
            allowAnomaly: options.allowAnomaly ?? false,
        });
        await client.initialize(workspace, {
            initializationOptions: options.initializationOptions,
        });
        return { client, workspace };
    };
    factory.spawn = spawnTracked;
    factory.tmpdir = (): Workspace => {
        const workspace = Workspace.tmp();
        tempDirs.push(workspace);
        return workspace;
    };
    factory.tmp = (options: SessionOptions = {}): Session => {
        const workspace = factory.tmpdir();
        const client = spawnTracked(workspace, options);
        return { client, workspace };
    };

    const teardown = async (failed: boolean): Promise<void> => {
        const teardownErrors: unknown[] = [];
        for (const session of opened.reverse()) {
            // The anomaly gate must run even when shutdown itself fails — a
            // crashed server is exactly when the anomaly evidence matters most.
            try {
                // A client the test already shut down explicitly (restart
                // tests) has passed its exit gate; don't shut it down twice.
                if (!session.client.disposed) {
                    await session.client.shutdown({ verbose: failed });
                }
            } catch (exc) {
                teardownErrors.push(exc);
            } finally {
                try {
                    if (!session.allowAnomaly) {
                        session.client.assertNoAnomaly(session.workspace?.root ?? null);
                    }
                } catch (exc) {
                    teardownErrors.push(exc);
                }
            }
        }
        // Directories go after every server is down: the anomaly gates
        // above read .clice/logs, and a live server may still write.
        for (const session of opened) {
            if (session.workspace !== null && !tempDirs.includes(session.workspace)) {
                session.workspace.rm(".clice");
            }
        }
        for (const dir of tempDirs.reverse()) {
            dir.remove();
        }
        releases.reverse().forEach((release) => {
            release();
        });
        if (teardownErrors.length > 0) {
            throw teardownErrors[0];
        }
    };

    return { session: factory, teardown };
}
