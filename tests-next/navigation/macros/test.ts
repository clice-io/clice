import { answers } from "@clice/tools/testing";

// Navigation reads open documents and the index, which clice inspect has
// neither of. A macro's name is found both ways: where it is defined, and
// every expansion, test and #undef of it.
answers({ paths: { server: "navigation reads open documents and the index" } }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, { definition: a.definition(m), references: a.references(m) });
    }
});
