import { answers } from "@clice/tools/testing";

answers(
    {
        indexing: true,
        paths: {
            separate:
                "the header comes from the project index, which clice inspect does not build: it offers nothing",
        },
    },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.codeActions(m, { compile: true }));
        }
    },
);
