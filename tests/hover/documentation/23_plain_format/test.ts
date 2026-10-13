import { answers } from "@clice/tools/testing";

answers("doxygen", async (a) => {
    await a.show("function", a.hover(a.marker("function")));
});

answers("plain", { config: { documentation: { format: "plain" } } }, async (a) => {
    await a.show("function", a.hover(a.marker("function")));
});
