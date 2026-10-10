/// Session machinery for the test suites: executable resolution and the
/// resource-managing session factory with its teardown gates (clean
/// shutdown, no anomalies). Framework-
/// agnostic — each suite binds it to vitest in its own thin fixture file
/// (tests/integration/fixtures.ts, tests/snap/fixtures.ts).

import * as fs from "node:fs";
import * as path from "node:path";
import { CliceClient, type InitializeOptions, type StartOptions } from "./client.ts";
import { Workspace } from "./workspace.ts";
import { logFiles } from "../process_gate.ts";

export function cliceExecutable(): string {
    let exe = process.env["CLICE_EXECUTABLE"];
    if (!exe) {
        throw new Error("CLICE_EXECUTABLE is not set; point it at build/<type>/bin/bin/clice");
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

const LOG_TAIL_LINES = 200;

function printLogTails(root: string | null): void {
    for (const file of logFiles(root)) {
        const lines = fs.readFileSync(file, "utf8").trimEnd().split("\n").slice(-LOG_TAIL_LINES);
        console.log(`--- last ${lines.length} lines of ${file}\n${lines.join("\n")}`);
    }
}

export interface Session {
    client: CliceClient;
    workspace: Workspace;
}

export interface SessionOptions extends InitializeOptions, StartOptions {
    /// The program to run instead of cliceExecutable(), such as a release
    /// build of it.
    executable?: string | undefined;
    /// Anomalies are internal clice bugs — every test session must end
    /// without one. Tests that intentionally trigger anomalies opt out here
    /// and assert on them explicitly.
    allowAnomaly?: boolean | undefined;
}

/// The session factory doubles as the test's resource manager — the
/// fixture-teardown equivalent of a destructor. Every server it spawns
/// and every temp workspace it mints is registered and reclaimed
/// automatically (shutdown gate, anomaly gate, removal), so tests never
/// write try/finally cleanup. A client already shut down explicitly via
/// client.shutdown() (restart tests) is skipped by the teardown.
export interface SessionFactory {
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

export function createSessionFactory(): SessionHandle {
    const opened: OpenedSession[] = [];
    const tempDirs: Workspace[] = [];

    const spawnTracked = (
        workspace: Workspace | null,
        options: SessionOptions = {},
    ): CliceClient => {
        const client = CliceClient.start(options.executable ?? cliceExecutable(), {
            drainStderr: options.drainStderr,
            args: options.args,
            env: options.env,
        });
        opened.push({
            client,
            workspace,
            allowAnomaly: options.allowAnomaly ?? false,
        });
        return client;
    };

    const tmpdir = (): Workspace => {
        const workspace = Workspace.tmp();
        tempDirs.push(workspace);
        return workspace;
    };
    const factory: SessionFactory = {
        spawn: spawnTracked,
        tmpdir,
        tmp: (options: SessionOptions = {}): Session => {
            const workspace = tmpdir();
            const client = spawnTracked(workspace, options);
            return { client, workspace };
        },
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
        // A failure only CI reproduces leaves nothing else to read once the
        // workspace below is deleted.
        if (failed) {
            for (const session of opened) {
                printLogTails(session.workspace?.root ?? null);
            }
        }
        // Directories go after every server is down: the anomaly gates
        // above read .clice/logs, and a live server may still write.
        for (const dir of tempDirs.reverse()) {
            dir.remove();
        }
        if (teardownErrors.length > 0) {
            throw teardownErrors[0];
        }
    };

    return { session: factory, teardown };
}
