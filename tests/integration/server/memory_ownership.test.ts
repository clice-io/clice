/// Ownership-gauge tests for memory lifecycle regressions.
///
/// Each leak class is pinned by a deterministic counter from the
/// clice/internal/stats hook instead of a brittle RSS assertion: shards flip
/// back to disk after a save, saves write only the true dirty set, and
/// cancelled builds leave no tmp blobs behind.

import * as fs from "node:fs";
import * as path from "node:path";
import {
    MTIME_GRANULARITY,
    assertNoAnomaly,
    sleep,
    waitForIndex,
    waitForRecompile,
} from "../../tools/checks.ts";
import type { CliceClient } from "../../tools/client.ts";
import { writeCdb } from "../../tools/compile_commands.ts";
import { expect, test } from "../../tools/fixtures.ts";

// Versioned root of the unified cache store; bump together with
// cache_format_version in src/server/state/workspace.h.
const CACHE_ROOT = path.join(".clice", "cache", "v4");

/// Read a numeric field from a stats response, or null if missing/non-number.
function getField(stats: unknown, key: string): number | null {
    if (stats !== null && typeof stats === "object" && key in stats) {
        const value = (stats as Record<string, unknown>)[key];
        return typeof value === "number" ? value : null;
    }
    return null;
}

/// Write a clice.toml that pins cache_dir to <workspace>/.clice/.
function pinCacheToWorkspace(workspace: string): void {
    fs.writeFileSync(
        path.join(workspace, "clice.toml"),
        '[project]\ncache_dir = "${workspace}/.clice"\n',
    );
}

/// In-flight tmp files of all store instances. Committed blobs appear
/// atomically, so anything under tmp/ is either an in-flight write of a live
/// server or crash residue awaiting cleanup.
function listTmpFiles(workspace: string): string[] {
    const tmpDir = path.join(workspace, CACHE_ROOT, "tmp");
    if (!fs.existsSync(tmpDir)) {
        return [];
    }
    return fs
        .readdirSync(tmpDir, { recursive: true, encoding: "utf8" })
        .map((name) => path.join(tmpDir, name))
        .filter((p) => fs.statSync(p).isFile())
        .sort();
}

/// Poll clice/internal/stats until predicate(stats) holds.
async function waitStats(
    client: CliceClient,
    predicate: (stats: unknown) => boolean,
    message = "",
): Promise<unknown> {
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
    pinCacheToWorkspace(workspace);
    const files: string[] = [];
    for (let i = 0; i < 4; i++) {
        const name = `file${i}.cpp`;
        fs.writeFileSync(path.join(workspace, name), `int func_${i}() { return ${i}; }\n`);
        files.push(name);
    }
    writeCdb(workspace, files);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "file0.cpp"));
    expect(await waitForIndex(client, uri, "func_3"), "background index did not finish").toBe(true);

    const stats = await waitStats(
        client,
        (s) => getField(s, "indexInmemoryShards") === 0,
        "shards did not flip back after save",
    );
    expect(
        getField(stats, "lastSaveShards"),
        `the settled round should have written shards: ${JSON.stringify(stats)}`,
    ).toBeGreaterThanOrEqual(1);
    assertNoAnomaly(client, workspace);
});

test("save writes only dirty shards", async ({ session }) => {
    const { client, workspace } = session.tmp();
    pinCacheToWorkspace(workspace);
    const files: string[] = [];
    for (let i = 0; i < 4; i++) {
        const name = `file${i}.cpp`;
        fs.writeFileSync(path.join(workspace, name), `int func_${i}() { return ${i}; }\n`);
        files.push(name);
    }
    writeCdb(workspace, files);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "file0.cpp"));
    expect(await waitForIndex(client, uri, "func_3"), "background index did not finish").toBe(true);
    await waitStats(
        client,
        (s) => getField(s, "indexInmemoryShards") === 0,
        "initial round did not settle",
    );
    // The first workspace tick only seeds the stat baseline.
    await client.poll("workspace");

    // Change one file on disk and tick the tracker: only its shard should
    // be re-merged and re-saved.
    await sleep(MTIME_GRANULARITY);
    fs.writeFileSync(path.join(workspace, "file2.cpp"), "int func_2_renamed() { return 2; }\n");
    await client.poll("workspace");
    expect(await waitForIndex(client, uri, "func_2_renamed"), "reindex did not land").toBe(true);

    const stats = await waitStats(
        client,
        (s) => getField(s, "indexInmemoryShards") === 0,
        "incremental round did not settle",
    );
    // Load-bearing assumptions for the exact count: the files are
    // standalone (no includes, so no header-shard fan-out) and only
    // background indexing merges shards (the open file's interactive
    // compile does not contribute one).
    expect(
        getField(stats, "lastSaveShards"),
        `an incremental save must write only the touched shard: ${JSON.stringify(stats)}`,
    ).toBe(1);

    // A round with nothing to write commits zero shards: saving the open
    // file schedules a round, but its shard is served by the session and
    // background indexing skips open files.
    client.save(uri);
    await waitStats(
        client,
        (s) => getField(s, "lastSaveShards") === 0,
        "a no-op round must save zero shards",
    );
    assertNoAnomaly(client, workspace);
});

test("cancel storm leaves no tmp", async ({ session }) => {
    const { client, workspace } = session.tmp();
    pinCacheToWorkspace(workspace);
    fs.writeFileSync(path.join(workspace, "header.h"), "#pragma once\nint base_val = 1;\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { return base_val; }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));

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

    await waitForRecompile(client, uri);
    await waitStats(
        client,
        (s) => getField(s, "pendingTmpFiles") === 0,
        "cancelled builds leaked tmp blobs",
    );
    expect(listTmpFiles(workspace), "tmp directory should be empty after settling").toEqual([]);
    // Shape smoke for the gauges no test asserts on: a missing or renamed
    // field reads back null, not a number.
    const stats = await client.stats();
    expect(
        getField(stats, "pchCacheEntries"),
        `a PCH was built: ${JSON.stringify(stats)}`,
    ).toBeGreaterThanOrEqual(1);
    expect(getField(stats, "sessions"), `one open document: ${JSON.stringify(stats)}`).toBe(1);
    assertNoAnomaly(client, workspace);
});

test("preamble state released", async ({ session }) => {
    const { client, workspace } = session.tmp();
    pinCacheToWorkspace(workspace);
    const names: string[] = [];
    for (let i = 0; i < 3; i++) {
        fs.writeFileSync(
            path.join(workspace, `h${i}.h`),
            `#pragma once\nint distinct_${i} = ${i};\n`,
        );
        fs.writeFileSync(
            path.join(workspace, `m${i}.cpp`),
            `#include "h${i}.h"\nint use_${i}() { return distinct_${i}; }\n`,
        );
        names.push(`m${i}.cpp`);
    }
    writeCdb(workspace, names);
    await client.initialize(workspace);

    const uris: string[] = [];
    for (let i = 0; i < 3; i++) {
        const [uri] = await client.openAndWait(path.join(workspace, `m${i}.cpp`));
        uris.push(uri);
    }
    let stats = await client.stats();
    expect(
        getField(stats, "pchLoadedStates"),
        `three distinct preambles: ${JSON.stringify(stats)}`,
    ).toBe(3);

    for (const uri of uris) {
        client.close(uri);
    }
    // Budget follows the open count (open + 2, the slack keeping a
    // just-closed state warm): with everything closed at least one of the
    // three states must unload instead of staying mapped forever.
    await waitStats(
        client,
        (s) => {
            const value = getField(s, "pchLoadedStates");
            return value !== null && value <= 2;
        },
        "closing documents must release loaded preamble states",
    );

    // Reload after unload: reopening must reopen the blob from disk and
    // keep serving queries against the preamble's symbols.
    const [uri0] = await client.openAndWait(path.join(workspace, "m0.cpp"));
    const hover = await client.hoverAt(uri0, 1, 25);
    expect(hover, "query must survive an unload/reload cycle").not.toBeNull();
    stats = await client.stats();
    expect(
        getField(stats, "pchLoadedStates"),
        `state reloaded: ${JSON.stringify(stats)}`,
    ).toBeGreaterThanOrEqual(1);
    assertNoAnomaly(client, workspace);
});

test("same preamble shared", async ({ session }) => {
    const { client, workspace } = session.tmp();
    pinCacheToWorkspace(workspace);
    fs.writeFileSync(path.join(workspace, "shared.h"), "#pragma once\nint shared_val = 1;\n");
    const names: string[] = [];
    for (let i = 0; i < 4; i++) {
        fs.writeFileSync(
            path.join(workspace, `s${i}.cpp`),
            `#include "shared.h"\nint fn_${i}() { return shared_val; }\n`,
        );
        names.push(`s${i}.cpp`);
    }
    writeCdb(workspace, names);
    await client.initialize(workspace);

    for (let i = 0; i < 4; i++) {
        await client.openAndWait(path.join(workspace, `s${i}.cpp`));
    }
    // Identical preambles share one content key, and sharing means one
    // blob: opening more consumers must not multiply loaded states.
    const stats = await client.stats();
    expect(getField(stats, "pchLoadedStates"), `one shared key: ${JSON.stringify(stats)}`).toBe(1);
    expect(getField(stats, "pchCacheEntries"), `one shared entry: ${JSON.stringify(stats)}`).toBe(
        1,
    );
    assertNoAnomaly(client, workspace);
});
