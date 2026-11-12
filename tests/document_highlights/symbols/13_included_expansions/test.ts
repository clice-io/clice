import { answers } from "@clice/tools/testing";

answers("open", async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.highlights(m));
    }
});

answers("indexed", { indexing: true }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.highlights(m));
    }
});
