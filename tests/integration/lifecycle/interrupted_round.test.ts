/// A background indexing round cut short: what the next start finds on disk.

import { runProcess, waitUntil } from "@clice/tools/client";
import { cliceExecutable, expect, test } from "../fixtures.ts";

const UNIT_COUNT = 24;

test("checkpoint survives a kill", async ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    const units = Array.from({ length: UNIT_COUNT }, (_, i) => `unit${i}.cpp`);
    for (const [i, unit] of units.entries()) {
        ws.write(unit, `#include <map>\n#include <string>\nint unit${i}() { return ${i}; }\n`);
    }
    ws.writeCDB(units);

    const client = session.spawn(ws, { env: { CLICE_TEST_CHECKPOINT_MS: "500" } });
    await client.initialize(ws);
    const log = () => client.drainedStderr().toString("utf8");
    await waitUntil(() => /phase=save shards=[1-9]/.test(log()), {
        timeout: 120_000,
        interval: 100,
        description: "a checkpoint committing shards",
    });
    // The kill must land mid-round, before the round-end save.
    expect(log()).not.toContain("[perf:index] phase=run ");
    client.killServer();
    client.dispose();

    const stats = await runProcess(
        cliceExecutable(),
        ["index", "--stats", "--workspace", ws.root],
        {
            timeout: 30_000,
        },
    );
    expect(stats.status, `stderr: ${stats.stderr}`).toBe(0);
    const persisted = Number(/Translation units: (\d+)/.exec(stats.stdout)?.[1]);
    expect(persisted).toBeGreaterThan(0);
    expect(persisted).toBeLessThan(UNIT_COUNT);
});
