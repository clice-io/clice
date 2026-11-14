import { answers } from "@clice/tools/testing";

answers({ config: { code_completion: { limit: 1 } } }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m));
    }
});
