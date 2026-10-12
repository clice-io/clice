import { answers } from "@clice/tools/testing";

// Navigation reads open documents and the index, which clice inspect has
// neither of.
answers({ paths: { server: "navigation reads open documents and the index" } }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.typeDefinition(m));
    }
});
