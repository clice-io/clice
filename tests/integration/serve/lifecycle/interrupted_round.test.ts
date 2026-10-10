/// A background indexing round cut short: what the next start finds on disk.

import * as proto from "vscode-languageserver-protocol";
import type { Hold, Serve } from "@clice/tools/actions";
import { sleep } from "@clice/tools/client";
import { expect, serve } from "../../fixtures.ts";

const UNIT_COUNT = 24;
const CHECKPOINT_MS = 500;

const FILES: Record<string, string> = {
    // The cache where `clice index` looks for it.
    "clice.toml": '[project]\ncache_dir = "${workspace}/.clice"\n',
};
for (let i = 0; i < UNIT_COUNT; i++) {
    FILES[`unit${i}.cpp`] = `#include <map>\n#include <string>\nint unit${i}() { return ${i}; }\n`;
}

/// The units the persisted index holds; the round must have been cut short.
async function persistedUnits(s: Serve): Promise<number> {
    const stats = await s.cli("index", "--stats");
    expect(stats.status, `stderr: ${stats.stderr}`).toBe(0);
    const persisted = Number(/Translation units: (\d+)/.exec(stats.stdout)?.[1]);
    expect(persisted).toBeLessThan(UNIT_COUNT);
    return persisted;
}

/// Hold the round open past a merged unit: of the units' index replies, the
/// first to come lands, and the next stays parked — the round cannot end
/// while it is.
async function midRound(s: Serve): Promise<void> {
    const holds: Hold[] = [];
    for (const unit of Object.keys(s.manifest.units)) {
        holds.push(await s.hold("index", unit));
    }
    const first = await s.firstParked(holds);
    await first.release();
    const rest = holds.filter((hold) => hold !== first);
    const kept = await s.firstParked(rest);
    for (const hold of rest) {
        if (hold !== kept) {
            await hold.release();
        }
    }
}

/// The master's log file, written before the server goes on: its stderr
/// mirror may lag behind the replies.
function serverLog(s: Serve): string {
    return s.workspace.log("master.log");
}

serve.files(FILES, { env: { CLICE_TEST_CHECKPOINT_MS: String(CHECKPOINT_MS) } })(
    "checkpoint survives a kill",
    async ({ s }) => {
        await midRound(s);
        // Checkpoints run on a timer: a few intervals hold one after the
        // merge.
        await sleep(3 * CHECKPOINT_MS);
        expect(serverLog(s)).toMatch(/phase=save shards=[1-9]/);
        // The kill must land mid-round, before the round-end save.
        expect(serverLog(s)).not.toContain("[perf:index] phase=run ");
        await s.kill();
        expect(await persistedUnits(s)).toBeGreaterThan(0);
    },
);

serve.files(FILES)("shutdown reply follows the save", async ({ s }) => {
    await midRound(s);
    expect(serverLog(s)).toContain("Merged TUIndex");
    // Zed kills the server as soon as it has sent `exit`.
    await s.client.sendRequest(proto.ShutdownRequest.type);
    await s.kill();
    expect(await persistedUnits(s)).toBeGreaterThan(0);
});

serve.files(FILES).skipIf(process.platform === "win32")(
    "sigterm saves the round",
    async ({ s }) => {
        await midRound(s);
        expect(serverLog(s)).toContain("Merged TUIndex");
        s.client.child.kill("SIGTERM");
        await s.client.assertExitedCleanly(30_000);
        s.client.dispose();
        expect(await persistedUnits(s)).toBeGreaterThan(0);
    },
);
