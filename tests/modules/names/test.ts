import { answers } from "@clice/tools/testing";

answers(
    {
        paths: {
            server: "clice inspect compiles one file alone, without the interfaces it imports",
        },
    },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.definition(m));
        }
    },
);
