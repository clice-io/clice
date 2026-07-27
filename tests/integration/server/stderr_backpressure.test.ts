/// A client that never drains stderr must not be able to wedge the server.

import * as fs from "node:fs";
import * as path from "node:path";
import { withTimeout } from "@clice/tools/client";
import { expect, test } from "../fixtures.ts";

const FLOOD_LINES = 3000;
const FLOOD_SIZE = 256;

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

test("log flood gated", async ({ session }) => {
    const workspace = session.tmpdir();
    // The load-generating hook must not exist for ordinary clients.
    workspace.write("probe.cpp", "int value = 42;\n");
    workspace.writeCDB(["probe.cpp"]);
    const client = session.spawn(workspace);
    await client.initialize(workspace);
    await expect(withTimeout(client.logFlood(1, 16), 10_000, "logFlood")).rejects.toThrow();
});

test("stderr flood never wedges", async ({ session }) => {
    // Editors drain stderr; a client that refuses to must cost mirror
    // lines — never liveness, and never file-log completeness. The volume
    // comes from a test hook so it is deterministic: feature log lines
    // change shape over time and must not be load-bearing here. Before the
    // fix the event loop parked in write(2) once the pipe filled (~1000
    // info lines in) and never answered again.
    const workspace = session.tmpdir();
    workspace.write("probe.cpp", "int value = 42;\n");
    workspace.writeCDB(["probe.cpp"]);

    const client = session.spawn(workspace, { drainStderr: false });
    await client.initialize(workspace, {
        initializationOptions: { project: { test_hooks: true } },
    });
    try {
        const [uri] = client.open("probe.cpp");
        // ~1MB in ten batches, far past the pipe (~196KB with asyncio's
        // reader buffer) plus the sink's 256KB buffer budget; each hover
        // in between is a bounded liveness probe.
        for (let batch = 0; batch < 10; batch++) {
            let hover: unknown;
            try {
                await withTimeout(
                    client.logFlood(Math.floor(FLOOD_LINES / 10), FLOOD_SIZE),
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
        // report the sink emits once writes flow again. This must happen
        // before the shutdown gate below, so it stays an explicit call.
        client.spawnStderrPump();
        await client.shutdown();
    }

    // Shedding happened where intended: the mirror lost flood lines and
    // reported the gap...
    const drained = client.drainedStderr().toString("utf8");
    expect(drained).toContain("client not draining");
    expect(floodLinesIn(drained)).toBeLessThan(FLOOD_LINES);

    // ...while the file log kept every single one: the mirror is
    // best-effort, the file log is the record.
    const fileText = readMasterLogs(workspace.path(path.join(".clice", "logs")));
    expect(floodLinesIn(fileText)).toBe(FLOOD_LINES);
}, 600_000);
