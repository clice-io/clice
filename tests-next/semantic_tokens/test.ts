import { answers } from "@clice/tools/testing";

answers(async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.semanticTokens(m));
    }
});
