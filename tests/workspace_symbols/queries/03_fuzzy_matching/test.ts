import { answers } from "@clice/tools/testing";

answers({ paths: { server: "workspace symbols read the project index" } }, async (a) => {
    for (const query of ["LinLis", "pconf", "pcfg"]) {
        await a.show(query, a.workspaceSymbols(query));
    }
});
