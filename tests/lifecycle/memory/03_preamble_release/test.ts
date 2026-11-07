import { test } from "@clice/tools/testing";

// Loaded preamble states live while a document's compile holds them:
// closing every document unloads them, and a document opened again reloads
// its state from disk and answers from it.

test(async (t) => {
    const s = await t.start();
    const files = ["m0.cpp", "m1.cpp", "m2.cpp"];
    for (const file of files) {
        await s.diagnostics(file);
    }
    await t.show("open", { pchLoadedIndexes: (await s.stats()).pchLoadedIndexes });
    for (const file of files) {
        s.close(file);
    }
    await s.sync();
    await t.show("closed", { pchLoadedIndexes: (await s.stats()).pchLoadedIndexes });
    await s.diagnostics("m0.cpp");
    await t.show("hover", s.hover(t.at("m0.cpp", "return |distinct_0")));
    await t.show("reopened", { pchLoadedIndexes: (await s.stats()).pchLoadedIndexes });
});
