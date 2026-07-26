/// Server lifecycle helpers: spawn, graceful shutdown, clean-exit gates.

import * as net from "node:net";
import * as proto from "vscode-languageserver-protocol";
import {
    CliceClient,
    SANITIZER_MARKERS,
    withTimeout,
    type InitializeOptions,
    type StartOptions,
} from "./client.ts";

let nextPortOffset = 0;

function tryBind(port: number): Promise<boolean> {
    return new Promise((resolve) => {
        const server = net.createServer();
        server.once("error", () => {
            resolve(false);
        });
        server.listen(port, "127.0.0.1", () => {
            server.close(() => {
                resolve(true);
            });
        });
    });
}

/// Pick a port from a per-worker range.
///
/// bind(0) draws from the kernel's shared pool: two concurrent workers can
/// grab the same port in the close-then-rebind gap. Disjoint per-worker
/// ranges (below the ephemeral range) remove that race; the advancing
/// offset avoids immediately reusing a just-released port.
export async function findFreePort(): Promise<number> {
    const index = Number(process.env["VITEST_POOL_ID"] ?? "0") || 0;
    const base = 21000 + index * 100;
    for (let i = 0; i < 100; i++) {
        const port = base + nextPortOffset;
        nextPortOffset = (nextPortOffset + 1) % 100;
        if (await tryBind(port)) {
            return port;
        }
    }
    throw new Error(`no free port in range ${base}-${base + 99}`);
}

/// Spawn a fresh clice server and initialize it. For multi-session tests.
export async function makeClient(
    executable: string,
    workspace: string,
    options: StartOptions & InitializeOptions = {},
): Promise<CliceClient> {
    const client = CliceClient.start(executable, options);
    await client.initialize(workspace, options);
    return client;
}

function serverStderrExcerpt(stderrText: string): string {
    const interesting = stderrText
        .split("\n")
        .filter(
            (line) =>
                line.includes("[warn]") ||
                line.includes("[error]") ||
                line.includes("Sanitizer") ||
                line.includes("==ERROR:") ||
                line.includes("runtime error:"),
        );
    return interesting.slice(-80).join("\n");
}

export async function assertServerExitedCleanly(
    client: CliceClient,
    timeout = 10_000,
): Promise<void> {
    const failures: string[] = [];

    if (client.child.exitCode === null && client.child.signalCode === null) {
        try {
            await withTimeout(client.exited, timeout, "server exit");
        } catch {
            client.child.kill("SIGKILL");
            await client.exited;
            failures.push(`server did not exit within ${timeout / 1000}s after shutdown`);
        }
    }

    console.log(`[server] exit code: ${client.child.exitCode}`);

    // Collect stderr AFTER the exit wait: exit-time output (sanitizer
    // reports, late crash text) must reach the scan below. The pump owns
    // the stream — wait for it to see EOF instead of racing it with a
    // second reader.
    try {
        await withTimeout(client.stderrEof, 2_000, "stderr EOF");
    } catch (exc) {
        // A pump that never saw EOF means the transcript below may be
        // partial — the sanitizer scan must not silently pass on it.
        failures.push(`stderr pump did not complete: ${String(exc)}`);
    }
    const stderrText = client.drainedStderr().toString("utf8");

    for (const line of serverStderrExcerpt(stderrText).split("\n")) {
        if (line) {
            console.log(`[server] ${line}`);
        }
    }

    if (client.child.exitCode !== 0) {
        failures.push(`server exited with code ${client.child.exitCode}`);
    }

    // A client that drained continuously must never see the drop report:
    // shedding under a live reader would mean ordinary tests silently lose
    // parts of the stderr transcript they later assert on.
    if (client.stderrDrainedFromStart && stderrText.includes("client not draining")) {
        failures.push("stderr mirror shed lines despite a draining client");
    }

    if (client.stderrMarkerHit !== null) {
        const excerpt = client.stderrMarkerHit.toString("utf8");
        failures.push(`server stderr contains sanitizer/runtime error output:\n${excerpt}`);
    } else if (SANITIZER_MARKERS.some((marker) => stderrText.includes(marker))) {
        failures.push("server stderr contains sanitizer/runtime error output");
    }

    if (failures.length > 0) {
        const excerpt = serverStderrExcerpt(stderrText);
        if (excerpt) {
            failures.push("server stderr excerpt:\n" + excerpt);
        }
        throw new Error(failures.join("\n"));
    }
}

/// Gracefully shut down a client, force-kill if needed.
export async function shutdownClient(
    client: CliceClient,
    options: { verbose?: boolean } = {},
): Promise<void> {
    try {
        await withTimeout(
            client.connection.sendRequest(proto.ShutdownRequest.type),
            10_000,
            "shutdown request",
        );
    } catch {
        // The exit-clean gate below reports the real failure.
    }

    try {
        await client.connection.sendNotification(proto.ExitNotification.type);
    } catch {
        // Connection may already be gone; the exit gate decides.
    }

    if (options.verbose && client.logMessages.length > 0) {
        const levels: Record<number, string> = { 1: "ERROR", 2: "WARN", 3: "INFO", 4: "LOG" };
        for (const msg of client.logMessages) {
            console.log(`[logMessage/${levels[msg.type] ?? "?"}] ${msg.message}`);
        }
    }

    try {
        await assertServerExitedCleanly(client);
    } finally {
        client.dispose();
    }
}
