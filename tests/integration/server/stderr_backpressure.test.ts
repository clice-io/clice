/// A client that never drains stderr must not be able to wedge the server.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { withTimeout } from "../../tools/client.ts";
import { writeCdb } from "../../tools/compile_commands.ts";
import { cliceExecutable, expect, test } from "../../tools/fixtures.ts";
import { makeClient, shutdownClient } from "../../tools/lifecycle.ts";

const FLOOD_LINES = 3000;
const FLOOD_SIZE = 256;

function mkTmp(): string {
    return fs.mkdtempSync(path.join(os.tmpdir(), "clice-test-"));
}

function floodLinesIn(text: string): number {
    return text.split("[stderr-flood ").length - 1;
}

/// Concatenate every master.log under a logs directory tree.
function readMasterLogs(logsDir: string): string {
    if (!fs.existsSync(logsDir)) {
        return "";
    }
    return fs
        .readdirSync(logsDir, { recursive: true, encoding: "utf8" })
        .filter((name) => path.basename(name) === "master.log")
        .map((name) => fs.readFileSync(path.join(logsDir, name), "utf8"))
        .join("");
}

test("log flood gated", async () => {
    const workspace = mkTmp();
    try {
        // The load-generating hook must not exist for ordinary clients.
        fs.writeFileSync(path.join(workspace, "probe.cpp"), "int value = 42;\n");
        writeCdb(workspace, ["probe.cpp"]);
        const client = await makeClient(cliceExecutable(), workspace);
        try {
            await expect(
                withTimeout(
                    client.connection.sendRequest("clice/internal/logFlood", {
                        count: 1,
                        size: 16,
                    }),
                    10_000,
                    "logFlood",
                ),
            ).rejects.toThrow();
        } finally {
            await shutdownClient(client);
        }
    } finally {
        fs.rmSync(workspace, { recursive: true, force: true });
    }
});

test("stderr flood never wedges", async () => {
    // Editors drain stderr; a client that refuses to must cost mirror
    // lines — never liveness, and never file-log completeness. The volume
    // comes from a test hook so it is deterministic: feature log lines
    // change shape over time and must not be load-bearing here. Before the
    // fix the event loop parked in write(2) once the pipe filled (~1000
    // info lines in) and never answered again.
    const workspace = mkTmp();
    try {
        fs.writeFileSync(path.join(workspace, "probe.cpp"), "int value = 42;\n");
        writeCdb(workspace, ["probe.cpp"]);

        const client = await makeClient(cliceExecutable(), workspace, {
            drainStderr: false,
            initializationOptions: { project: { test_hooks: true } },
        });
        try {
            const [uri] = client.open(path.join(workspace, "probe.cpp"));
            // ~1MB in ten batches, far past the pipe (~196KB with asyncio's
            // reader buffer) plus the sink's 256KB buffer budget; each hover
            // in between is a bounded liveness probe.
            for (let batch = 0; batch < 10; batch++) {
                let hover: unknown;
                try {
                    await withTimeout(
                        client.connection.sendRequest("clice/internal/logFlood", {
                            count: Math.floor(FLOOD_LINES / 10),
                            size: FLOOD_SIZE,
                        }),
                        18_000,
                        "logFlood",
                    );
                    hover = await withTimeout(client.hoverAt(uri, 0, 5), 18_000, "hover");
                } catch {
                    // Kill the wedged server first: a graceful teardown against
                    // a process that no longer reads stdin has nothing to wait
                    // for.
                    client.killServer();
                    throw new Error(`server wedged in batch ${batch} (stderr backpressure)`);
                }
                expect(hover, `empty hover in batch ${batch}`).not.toBeNull();
            }
        } finally {
            // Hostile phase over: resume draining so teardown can observe pipe
            // EOF (asyncio's Process.wait() waits on it) and collect the gap
            // report the sink emits once writes flow again.
            client.spawnStderrPump();
            await shutdownClient(client);
        }

        // Shedding happened where intended: the mirror lost flood lines and
        // reported the gap...
        const drained = client.drainedStderr().toString("utf8");
        expect(drained).toContain("client not draining");
        expect(floodLinesIn(drained)).toBeLessThan(FLOOD_LINES);

        // ...while the file log kept every single one: the mirror is
        // best-effort, the file log is the record.
        const logsDir = path.join(workspace, ".clice", "logs");
        const fileText = readMasterLogs(logsDir);
        expect(floodLinesIn(fileText)).toBe(FLOOD_LINES);
    } finally {
        fs.rmSync(workspace, { recursive: true, force: true });
    }
}, 600_000);
