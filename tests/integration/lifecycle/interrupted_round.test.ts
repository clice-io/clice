/// A background indexing round cut short: what the next start finds on disk.

import * as proto from "vscode-languageserver-protocol";
import { runProcess, waitUntil, type CliceClient } from "@clice/tools/client";
import type { Workspace } from "@clice/tools/workspace";
import { cliceExecutable, expect, test } from "../fixtures.ts";

const UNIT_COUNT = 24;

function slowUnits(ws: Workspace): void {
    ws.pinCacheDir();
    const units = Array.from({ length: UNIT_COUNT }, (_, i) => `unit${i}.cpp`);
    for (const [i, unit] of units.entries()) {
        ws.write(unit, `#include <map>\n#include <string>\nint unit${i}() { return ${i}; }\n`);
    }
    ws.writeCDB(units);
}

function serverLog(client: CliceClient): string {
    return client.drainedStderr().toString("utf8");
}

async function waitForLog(client: CliceClient, pattern: RegExp, description: string) {
    await waitUntil(() => pattern.test(serverLog(client)), {
        timeout: 120_000,
        interval: 100,
        description,
    });
}

/// The units the persisted index holds; the round must have been cut short.
async function persistedUnits(ws: Workspace): Promise<number> {
    const stats = await runProcess(
        cliceExecutable(),
        ["index", "--stats", "--workspace", ws.root],
        { timeout: 30_000 },
    );
    expect(stats.status, `stderr: ${stats.stderr}`).toBe(0);
    const persisted = Number(/Translation units: (\d+)/.exec(stats.stdout)?.[1]);
    expect(persisted).toBeLessThan(UNIT_COUNT);
    return persisted;
}

test("checkpoint survives a kill", async ({ session }) => {
    const ws = session.tmpdir();
    slowUnits(ws);
    const client = session.spawn(ws, { env: { CLICE_TEST_CHECKPOINT_MS: "500" } });
    await client.initialize(ws);
    await waitForLog(client, /phase=save shards=[1-9]/, "a checkpoint committing shards");
    // The kill must land mid-round, before the round-end save.
    expect(serverLog(client)).not.toContain("[perf:index] phase=run ");
    client.killServer();
    client.dispose();
    expect(await persistedUnits(ws)).toBeGreaterThan(0);
});

test("shutdown reply follows the save", async ({ session }) => {
    const ws = session.tmpdir();
    slowUnits(ws);
    const client = session.spawn(ws);
    await client.initialize(ws);
    await waitForLog(client, /Merged TUIndex/, "a merged unit");
    // Zed kills the server as soon as it has sent `exit`.
    await client.sendRequest(proto.ShutdownRequest.type);
    client.killServer();
    client.dispose();
    expect(await persistedUnits(ws)).toBeGreaterThan(0);
});

test.skipIf(process.platform === "win32")("sigterm saves the round", async ({ session }) => {
    const ws = session.tmpdir();
    slowUnits(ws);
    const client = session.spawn(ws);
    await client.initialize(ws);
    await waitForLog(client, /Merged TUIndex/, "a merged unit");
    client.child.kill("SIGTERM");
    await client.assertExitedCleanly(30_000);
    client.dispose();
    expect(await persistedUnits(ws)).toBeGreaterThan(0);
});
