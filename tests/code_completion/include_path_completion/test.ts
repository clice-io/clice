import { answers } from "@clice/tools/testing";

answers(
    { paths: { server: "include-path completion is answered before anything compiles" } },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.completion(m), { parts: ["insert", "edit"] });
        }
    },
);
