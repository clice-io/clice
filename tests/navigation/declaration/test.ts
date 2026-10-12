import { answers } from "@clice/tools/testing";

answers({ paths: { server: "navigation reads open documents and the index" } }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.declaration(m));
    }
});
