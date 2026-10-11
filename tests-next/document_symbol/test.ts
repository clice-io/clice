import { answers } from "@clice/tools/testing";

// Two views of one reply: the outline as an editor lists it, each symbol
// with its detail; and each symbol's whole extent, which breadcrumbs and
// sticky scroll read. Apart, a wrong extent does not flag the outline.
answers(async (a) => {
    await a.show("outline", a.documentSymbols("main.cpp"), { parts: ["detail"] });
    await a.show("extents", a.documentSymbols("main.cpp"), { parts: ["range"] });
});
