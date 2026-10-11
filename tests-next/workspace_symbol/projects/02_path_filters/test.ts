import { answers } from "@clice/tools/testing";

// A path filter matches from the workspace root: src/ is not lib/src/.
answers(
    { paths: { server: "workspace symbols read the project index" }, indexing: true },
    async (a) => {
        for (const query of ["sum path:src/", "sum path:lib/src/", "sum path:b.cpp"]) {
            await a.show(query, a.workspaceSymbols(query));
        }
    },
);
