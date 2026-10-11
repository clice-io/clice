import { answers } from "@clice/tools/testing";

answers(
    { paths: { server: "navigation reads open documents and the index" }, indexing: true },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, { definition: a.definition(m), declaration: a.declaration(m) });
        }
    },
);
