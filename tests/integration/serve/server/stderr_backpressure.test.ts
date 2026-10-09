/// A client that never drains stderr must not be able to wedge the server.

import { withTimeout } from "@clice/tools/client";
import { expect, serve } from "../../fixtures.ts";

const FLOOD_LINES = 3000;
const FLOOD_SIZE = 256;

function floodLinesIn(text: string): number {
    return text.split("[stderr-flood ").length - 1;
}

const PROBE = { "probe.cpp": "int value = 42;\n" };

serve.files(PROBE, { config: { project: { test_hooks: false } } })(
    "log flood gated",
    async ({ s }) => {
        // The load-generating hook must not exist for ordinary clients.
        await expect(s.client.logFlood(1, 16)).rejects.toThrow();
    },
);

serve.files(PROBE)("stderr flood never wedges", async ({ s }) => {
    // Editors drain stderr; a client that refuses to must cost mirror
    // lines — never liveness, and never file-log completeness. The volume
    // comes from a test hook so it is deterministic: feature log lines
    // change shape over time and must not be load-bearing here. Before the
    // fix the event loop parked in write(2) once the pipe filled (~1000
    // info lines in) and never answered again.
    await s.stop();
    const client = s.session.spawn(s.workspace, { drainStderr: false });
    await client.initialize(s.workspace);
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
    const fileText = s.workspace.log("master.log");
    expect(floodLinesIn(fileText)).toBe(FLOOD_LINES);
});
