import { test } from "@clice/tools/testing";

// Identical preambles share one content key, and sharing means one blob:
// more documents with it load no more states.

test(async (t) => {
    const s = await t.start();
    for (const file of ["s0.cpp", "s1.cpp", "s2.cpp", "s3.cpp"]) {
        await s.diagnostics(file);
    }
    const { pchLoadedStates, pchCacheEntries } = await s.stats();
    await t.show("four documents", { pchLoadedStates, pchCacheEntries });
});
