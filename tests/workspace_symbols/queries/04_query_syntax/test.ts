import { answers } from "@clice/tools/testing";

answers({ paths: { server: "workspace symbols read the project index" } }, async (a) => {
    for (const query of ['"process"', "proc*", "io::*", "io::**", "process kind:function"]) {
        await a.show(query, a.workspaceSymbols(query));
    }
});
