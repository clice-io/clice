import { answers } from "@clice/tools/testing";

answers(
    {
        paths: { server: "include-path completion is answered before anything compiles" },
        config: { code_completion: { limit: 1 } },
    },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.completion(m));
        }
    },
);
