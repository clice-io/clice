import { test } from "@clice/tools/testing";

// Loaded preamble states follow the open documents (their count and two
// more, kept warm): closing every document unloads some, and a document
// opened again reloads its state from disk and answers from it.

test(async (t) => {
    const s = await t.start();
    const files = ["m0.cpp", "m1.cpp", "m2.cpp"];
    for (const file of files) {
        await s.diagnostics(file);
    }
    await t.show("open", { pchLoadedStates: (await s.stats()).pchLoadedStates });
    for (const file of files) {
        s.close(file);
    }
    await s.sync();
    await t.show("closed", { pchLoadedStates: (await s.stats()).pchLoadedStates });
    await s.diagnostics("m0.cpp");
    await t.show("hover", s.hover(t.at("m0.cpp", "return |distinct_0")));
    await t.show("reopened", { pchLoadedStates: (await s.stats()).pchLoadedStates });
});
