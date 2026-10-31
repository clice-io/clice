import { expect } from "vitest";
import { protocolKeys, type StatsResult } from "@clice/tools/protocol";
import { test } from "@clice/tools/testing";

// Each edit changes the preamble while its build runs, so each build is
// superseded mid-way under a fresh content key: none leaves a temporary
// blob behind.

test(async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    let previous: Awaited<ReturnType<typeof s.park>> | undefined;
    for (let i = 0; i < 15; i++) {
        const park = await s.park("buildPch", "main.cpp", { at: "worker" });
        s.edit("main.cpp", { text: `#define STORM ${String(i)}\n${t.disk.read("main.cpp")}` });
        const diagnostics = s.diagnostics("main.cpp");
        await park.reached();
        await previous?.release();
        previous = park;
        if (i === 14) {
            await park.release();
            await t.show("diagnostics", diagnostics);
        } else {
            void diagnostics.catch(() => undefined);
        }
    }
    await s.sync();
    const stats = await s.stats();
    await t.show("after the storm", {
        pendingTmpFiles: stats.pendingTmpFiles,
        pchCacheEntries: stats.pchCacheEntries,
        sessions: stats.sessions,
    });
    expect(t.disk.list(".clice/cache/*/tmp/**")).toEqual([]);
    // The C++ and TS shapes of the reply are written by hand on both
    // sides: a gauge added to one and not the other shows here.
    expect(Object.keys(stats).sort()).toEqual(
        [
            ...protocolKeys<StatsResult>()([
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
                "workers",
            ]),
        ].sort(),
    );
});
