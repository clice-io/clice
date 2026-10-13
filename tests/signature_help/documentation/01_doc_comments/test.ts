import { answers } from "@clice/tools/testing";

answers("shown", async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.signatureHelp(m));
    }
});

answers("hidden", { config: { signature_help: { documentation: false } } }, async (a) => {
    await a.show("parameter", a.signatureHelp(a.marker("parameter")));
});
