/// Ownership-gauge tests for memory lifecycle regressions.
///
/// Each leak class is pinned by a deterministic counter from the
/// clice/internal/stats hook instead of a brittle RSS assertion: shards flip
/// back to disk after a save, saves write only the true dirty set, and
/// cancelled builds leave no tmp blobs behind.

import { MTIME_GRANULARITY, sleep, type CliceClient } from "@clice/tools/client";
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

test("shards flip back after save", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.pinCacheDir();
    const files: string[] = [];
    for (let i = 0; i < 4; i++) {
        const name = `file${i}.cpp`;
        workspace.write(name, `int func_${i}() { return ${i}; }\n`);
        files.push(name);
    }
    workspace.writeCDB(files);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait("file0.cpp");
    expect(await client.waitForIndex(uri, "func_3"), "background index did not finish").toBe(true);

    const stats = await waitStats(
        client,
        (s) => s.indexInmemoryShards === 0,
        "shards did not flip back after save",
    );
    expect(
        stats.lastSaveShards,
        `the settled round should have written shards: ${JSON.stringify(stats)}`,
    ).toBeGreaterThanOrEqual(1);
    client.assertNoAnomaly();
});

test("save writes only dirty shards", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.pinCacheDir();
    const files: string[] = [];
    for (let i = 0; i < 4; i++) {
        const name = `file${i}.cpp`;
        workspace.write(name, `int func_${i}() { return ${i}; }\n`);
        files.push(name);
    }
    workspace.writeCDB(files);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait("file0.cpp");
    expect(await client.waitForIndex(uri, "func_3"), "background index did not finish").toBe(true);
    await waitStats(client, (s) => s.indexInmemoryShards === 0, "initial round did not settle");
    // The first workspace tick only seeds the stat baseline.
    await client.poll("workspace");

    // Change one file on disk and tick the tracker: only its shard should
    // be re-merged and re-saved.
    await sleep(MTIME_GRANULARITY);
    workspace.write("file2.cpp", "int func_2_renamed() { return 2; }\n");
    await client.poll("workspace");
    expect(await client.waitForIndex(uri, "func_2_renamed"), "reindex did not land").toBe(true);

    const stats = await waitStats(
        client,
        (s) => s.indexInmemoryShards === 0,
        "incremental round did not settle",
    );
    // Load-bearing assumptions for the exact count: the files are
    // standalone (no includes, so no header-shard fan-out) and only
    // background indexing merges shards (the open file's interactive
    // compile does not contribute one).
    expect(
        stats.lastSaveShards,
        `an incremental save must write only the touched shard: ${JSON.stringify(stats)}`,
    ).toBe(1);

    // A round with nothing to write commits zero shards: saving the open
    // file schedules a round, but its shard is served by the session and
    // background indexing skips open files.
    client.save(uri);
    await waitStats(client, (s) => s.lastSaveShards === 0, "a no-op round must save zero shards");
    client.assertNoAnomaly();
});

test("cancel storm leaves no tmp", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.pinCacheDir();
    workspace.write("header.h", "#pragma once\nint base_val = 1;\n");
    workspace.write("main.cpp", '#include "header.h"\nint main() { return base_val; }\n');
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait("main.cpp");

    // Each edit changes the preamble text, so each supersedes the previous
    // PCH build under a fresh content key.
    for (let i = 0; i < 15; i++) {
        client.change(
            uri,
            i + 2,
            `#define STORM ${i}\n#include "header.h"\nint main() { return base_val; }\n`,
        );
        await sleep(50);
    }

    await client.waitForRecompile(uri);
    await waitStats(client, (s) => s.pendingTmpFiles === 0, "cancelled builds leaked tmp blobs");
    expect(workspace.tmpFiles(), "tmp directory should be empty after settling").toEqual([]);
    // Extra gauges no other test asserts on: a real PCH build and one open
    // session must both register.
    const stats = await client.stats();
    expect(
        stats["pchCacheEntries"],
        `a PCH was built: ${JSON.stringify(stats)}`,
    ).toBeGreaterThanOrEqual(1);
    expect(stats["sessions"], `one open document: ${JSON.stringify(stats)}`).toBe(1);
    client.assertNoAnomaly();
});

test("preamble state released", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.pinCacheDir();
    const names: string[] = [];
    for (let i = 0; i < 3; i++) {
        workspace.write(`h${i}.h`, `#pragma once\nint distinct_${i} = ${i};\n`);
        workspace.write(
            `m${i}.cpp`,
            `#include "h${i}.h"\nint use_${i}() { return distinct_${i}; }\n`,
        );
        names.push(`m${i}.cpp`);
    }
    workspace.writeCDB(names);
    await client.initialize(workspace);

    const uris: string[] = [];
    for (let i = 0; i < 3; i++) {
        const [uri] = await client.openAndWait(`m${i}.cpp`);
        uris.push(uri);
    }
    let stats = await client.stats();
    expect(stats.pchLoadedStates, `three distinct preambles: ${JSON.stringify(stats)}`).toBe(3);

    for (const uri of uris) {
        client.close(uri);
    }
    // Budget follows the open count (open + 2, the slack keeping a
    // just-closed state warm): with everything closed at least one of the
    // three states must unload instead of staying mapped forever.
    await waitStats(
        client,
        (s) => s.pchLoadedStates <= 2,
        "closing documents must release loaded preamble states",
    );

    // Reload after unload: reopening must reopen the blob from disk and
    // keep serving queries against the preamble's symbols.
    const [uri0] = await client.openAndWait("m0.cpp");
    const hover = await client.hoverAt(uri0, 1, 25);
    expect(hover, "query must survive an unload/reload cycle").not.toBeNull();
    stats = await client.stats();
    expect(
        stats.pchLoadedStates,
        `state reloaded: ${JSON.stringify(stats)}`,
    ).toBeGreaterThanOrEqual(1);
    client.assertNoAnomaly();
});

test("same preamble shared", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.pinCacheDir();
    workspace.write("shared.h", "#pragma once\nint shared_val = 1;\n");
    const names: string[] = [];
    for (let i = 0; i < 4; i++) {
        workspace.write(`s${i}.cpp`, `#include "shared.h"\nint fn_${i}() { return shared_val; }\n`);
        names.push(`s${i}.cpp`);
    }
    workspace.writeCDB(names);
    await client.initialize(workspace);

    for (let i = 0; i < 4; i++) {
        await client.openAndWait(`s${i}.cpp`);
    }
    // Identical preambles share one content key, and sharing means one
    // blob: opening more consumers must not multiply loaded states.
    const stats = await client.stats();
    expect(stats.pchLoadedStates, `one shared key: ${JSON.stringify(stats)}`).toBe(1);
    expect(stats["pchCacheEntries"], `one shared entry: ${JSON.stringify(stats)}`).toBe(1);
    client.assertNoAnomaly();
});
