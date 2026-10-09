/// Ownership-gauge tests for memory lifecycle regressions.
///
/// Each leak class is pinned by a deterministic counter from the
/// clice/internal/stats hook instead of a brittle RSS assertion: shards flip
/// back to disk after a save, saves write only the true dirty set, and
/// cancelled builds leave no tmp blobs behind.

import type { CliceClient } from "@clice/tools/client";
import { wireKeys, type StatsResult } from "@clice/tools/protocol";
import { at, expect, serve } from "../../fixtures.ts";

const FUNCS = Object.fromEntries(
    [0, 1, 2, 3].map((i) => [`file${i}.cpp`, `int func_${i}() { return ${i}; }\n`]),
);

serve.files(FUNCS)("shards flip back after save", async ({ s }) => {
    await s.compiled("file0.cpp");
    await s.indexed();
    expect(s.show(await s.workspaceSymbols("func_3")), "background index did not finish").toBe(
        "func_3 file3.cpp: int func_3() { return 3; }",
    );

    const stats = await s.client.stats();
    expect(stats.indexInmemoryShards, "shards did not flip back after save").toBe(0);
    expect(
        stats.lastSaveShards,
        `the settled round should have written shards: ${JSON.stringify(stats)}`,
    ).toBeGreaterThanOrEqual(1);
    s.client.assertNoAnomaly();
});

/// The server settled, and the names its index has for `query`.
async function settled(client: CliceClient, query: string, poll = false): Promise<string[]> {
    expect((await client.sync({ poll })).pending).toEqual([]);
    return ((await client.workspaceSymbols(query)) ?? []).map((symbol) => symbol.name);
}

serve.files(FUNCS)("save writes only dirty shards", async ({ s }) => {
    // Open before the project starts: its first round leaves the open file
    // to the file's own compile. The case's server has run a round of its
    // own, so a server spawned here starts from no cache.
    await s.stop();
    s.workspace.rm(".clice/cache");
    const client = s.session.spawn(s.workspace);
    let uri = "";
    await client.initialize(s.workspace, {
        beforeInitialized: () => {
            [uri] = client.open("file0.cpp");
            return Promise.resolve();
        },
    });
    await client.pullDiagnostics(uri);
    expect(await settled(client, "func_3"), "background index did not finish").toContain("func_3");
    expect((await client.stats()).indexInmemoryShards, "initial round did not settle").toBe(0);

    // Change one file on disk and tick the tracker: only its shard should
    // be re-merged and re-saved. The rewrite has another size, which the
    // look at the disk sees at once.
    s.disk.write("file2.cpp", "int func_2_renamed() { return 2; }\n");
    expect(await settled(client, "func_2_renamed", true), "reindex did not land").toContain(
        "func_2_renamed",
    );

    const stats = await client.stats();
    expect(stats.indexInmemoryShards, "incremental round did not settle").toBe(0);
    // Load-bearing assumptions for the exact count: the files are
    // standalone (no includes, so no header-shard fan-out) and only
    // background indexing merges shards (the open file's interactive
    // compile does not contribute one).
    expect(
        stats.lastSaveShards,
        `an incremental save must write only the touched shard: ${JSON.stringify(stats)}`,
    ).toBe(1);

    // The open file compiles itself, so the rounds leave its disk snapshot
    // alone and saving the same bytes queues nothing; closing it hands the
    // file back to the background index.
    client.save(uri);
    expect((await client.sync()).pending).toEqual([]);
    const saved = await client.stats();
    expect(saved.indexInmemoryShards, "the save left shards in memory").toBe(0);
    expect(saved.indexShardContentBytes).toBe(stats.indexShardContentBytes);

    client.close(uri);
    expect((await client.sync()).pending).toEqual([]);
    const closed = await client.stats();
    expect(closed.indexShardContentBytes, "the closed file's shard did not land").toBeGreaterThan(
        saved.indexShardContentBytes,
    );
    expect(closed.indexInmemoryShards, "the closed file's shard did not land").toBe(0);
    client.assertNoAnomaly();
});

serve.files({
    "header.h": "#pragma once\nint base_val = 1;\n",
    "main.cpp": '#include "header.h"\nint main() { return base_val; }\n',
})("cancel storm leaves no tmp", async ({ s }) => {
    await s.compiled("main.cpp");

    // Each edit changes the preamble text, and the pull after it starts a
    // build of it, so each supersedes the previous PCH build under a fresh
    // content key.
    for (let i = 0; i < 15; i++) {
        s.edit("main.cpp", {
            text: `#define STORM ${i}\n#include "header.h"\nint main() { return base_val; }\n`,
        });
        void s.diagnostics("main.cpp").catch(() => undefined);
    }

    await s.compiled("main.cpp");
    await s.sync();
    let stats = await s.client.stats();
    expect(stats.pendingTmpFiles, "cancelled builds leaked tmp blobs").toBe(0);
    expect(s.workspace.tmpFiles(), "tmp directory should be empty after settling").toEqual([]);
    // Extra gauges no other test asserts on: a real PCH build and one open
    // session must both register.
    stats = await s.client.stats();
    expect(
        stats.pchCacheEntries,
        `a PCH was built: ${JSON.stringify(stats)}`,
    ).toBeGreaterThanOrEqual(1);
    expect(stats.sessions, `one open document: ${JSON.stringify(stats)}`).toBe(1);
    // The C++ and TS shapes of the reply are hand-written on both sides; a
    // gauge added to one and not the other shows up here.
    expect(Object.keys(stats).sort()).toEqual(
        [
            ...wireKeys<StatsResult>()([
                "builds",
                "checksLooked",
                "checksTrusted",
                "importScans",
                "headerContexts",
                "indexIdle",
                "synthesizedContexts",
                "indexInmemoryShards",
                "indexShardContentBytes",
                "lastSaveShards",
                "pchCacheEntries",
                "pchLoadedStates",
                "pchStateBytes",
                "pendingTmpFiles",
                "sessions",
            ]),
        ].sort(),
    );
    s.client.assertNoAnomaly();
});

const PREAMBLES: Record<string, string> = {};
for (let i = 0; i < 3; i++) {
    PREAMBLES[`h${i}.h`] = `#pragma once\nint distinct_${i} = ${i};\n`;
    PREAMBLES[`m${i}.cpp`] = `#include "h${i}.h"\nint use_${i}() { return distinct_${i}; }\n`;
}

serve.files(PREAMBLES)("preamble state released", async ({ s }) => {
    const units = ["m0.cpp", "m1.cpp", "m2.cpp"];
    for (const unit of units) {
        await s.compiled(unit);
    }
    let stats = await s.client.stats();
    expect(stats.pchLoadedStates, `three distinct preambles: ${JSON.stringify(stats)}`).toBe(3);

    for (const unit of units) {
        s.close(unit);
    }
    // Budget follows the open count (open + 2, the slack keeping a
    // just-closed state warm): with everything closed at least one of the
    // three states must unload instead of staying mapped forever.
    await s.sync();
    stats = await s.client.stats();
    expect(
        stats.pchLoadedStates,
        "closing documents must release loaded preamble states",
    ).toBeLessThanOrEqual(2);

    // Reload after unload: reopening must reopen the blob from disk and
    // keep serving queries against the preamble's symbols.
    await s.compiled("m0.cpp");
    const hover = await s.hover(at("m0.cpp", "return dist|inct_0"));
    expect(hover, "query must survive an unload/reload cycle").not.toBeNull();
    stats = await s.client.stats();
    expect(
        stats.pchLoadedStates,
        `state reloaded: ${JSON.stringify(stats)}`,
    ).toBeGreaterThanOrEqual(1);
    s.client.assertNoAnomaly();
});

serve.files({
    "shared.h": "#pragma once\nint shared_val = 1;\n",
    ...Object.fromEntries(
        [0, 1, 2, 3].map((i) => [
            `s${i}.cpp`,
            `#include "shared.h"\nint fn_${i}() { return shared_val; }\n`,
        ]),
    ),
})("same preamble shared", async ({ s }) => {
    for (let i = 0; i < 4; i++) {
        await s.compiled(`s${i}.cpp`);
    }
    // Identical preambles share one content key, and sharing means one
    // blob: opening more consumers must not multiply loaded states.
    const stats = await s.client.stats();
    expect(stats.pchLoadedStates, `one shared key: ${JSON.stringify(stats)}`).toBe(1);
    expect(stats.pchCacheEntries, `one shared entry: ${JSON.stringify(stats)}`).toBe(1);
    s.client.assertNoAnomaly();
});
