import { answers } from "@clice/tools/testing";

answers(
    { paths: { server: "clice inspect compiles one file alone, and main.cppm imports a module" } },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.selectionRange(m));
        }
    },
);
