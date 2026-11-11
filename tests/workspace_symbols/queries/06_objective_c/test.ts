import { answers } from "@clice/tools/testing";

answers(
    { paths: { server: "workspace symbols read the project index" }, indexing: true },
    async (a) => {
        for (const query of [
            "Shape",
            "setX",
            "Drawable",
            "count",
            "area",
            "Object",
            "_generation",
        ]) {
            await a.show(query, a.workspaceSymbols(query));
        }
    },
);
