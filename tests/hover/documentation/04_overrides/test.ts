import { answers } from "@clice/tools/testing";

answers("inherited", async (a) => {
    await a.show("override", a.hover(a.marker("override")));
});

answers("not inherited", { config: { documentation: { inherit: false } } }, async (a) => {
    await a.show("override", a.hover(a.marker("override")));
});
