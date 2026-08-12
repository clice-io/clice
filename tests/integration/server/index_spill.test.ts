/// Oversized TUIndex results travel via a store tmp file instead of the
/// IPC frame. With a tiny inline limit every background-index result
/// spills; cross-file references prove the master verified and merged the
/// spilled bytes, and the transfer files must not outlive their requests.

import * as fs from "node:fs";
import * as path from "node:path";

import { sleep, type CliceClient } from "@clice/tools/client";
import type { StatsResult } from "@clice/tools/protocol";
import { expect, test } from "../fixtures.ts";

/// Poll clice/internal/stats until predicate(stats) holds.
async function waitStats(
    client: CliceClient,
    predicate: (stats: StatsResult) => boolean,
    message = "",
): Promise<StatsResult> {
    const deadline = Date.now() + 30_000;
    for (;;) {
        const stats = await client.stats();
        if (predicate(stats)) {
            return stats;
        }
        if (Date.now() > deadline) {
            throw new Error(`${message || "stats condition"} not met: ${JSON.stringify(stats)}`);
        }
        await sleep(200);
    }
}

function masterLog(logsDir: string): string {
    if (!fs.existsSync(logsDir)) {
        return "";
    }
    return fs
        .readdirSync(logsDir, { recursive: true, encoding: "utf8" })
        .filter((name) => path.basename(name) === "master.log")
        .map((name) => fs.readFileSync(path.join(logsDir, name), "utf8"))
        .join("\n");
}

test("index spill roundtrip", async ({ session }) => {
    const { client, workspace } = session.tmp();
    // The reference probe excludes declarations, so lib.cpp needs a real
    // call site for it to show up in the reference list.
    workspace.write(
        "lib.cpp",
        "int shared_fn() { return 42; }\nint call_it() { return shared_fn(); }\n",
    );
    workspace.write("main.cpp", "int shared_fn();\nint main() { return shared_fn(); }\n");
    workspace.writeCDB(["lib.cpp", "main.cpp"]);
    workspace.pinCacheDir();
    await client.initialize(workspace, {
        initializationOptions: {
            project: { idle_timeout_ms: 0, index_inline_limit: 1 },
        },
    });

    const [uri] = await client.openAndWait("main.cpp");
    expect(
        await client.waitForReference(uri, 0, 4, workspace.uri("lib.cpp")),
        "cross-file reference through spilled index",
    ).toBe(true);

    expect(masterLog(workspace.path(".clice/logs"))).toContain("transfer=file");

    await waitStats(client, (s) => s.pendingTmpFiles === 0, "transfer files leaked");
    expect(workspace.tmpFiles(), "tmp directory should be empty after settling").toEqual([]);
});
