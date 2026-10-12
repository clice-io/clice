import { answers } from "@clice/tools/testing";

answers("default", async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.hover(m));
    }
});

answers("configured", { config: { hover: { show_aka: false } } }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.hover(m));
    }
});
