import { answers } from "@clice/tools/testing";

answers("default", async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.hover(m));
    }
});

answers("configured", { config: { hover: { max_members: 2 } } }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.hover(m));
    }
});

answers("disabled", { config: { hover: { max_members: 0 } } }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.hover(m));
    }
});
