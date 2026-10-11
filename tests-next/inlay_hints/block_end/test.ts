import { answers } from "@clice/tools/testing";

answers("default", (a) => a.show("main.cpp", a.inlayHints("main.cpp")));
answers("configured", { config: { inlay_hints: { block_end: true } } }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.inlayHints(m));
    }
});
