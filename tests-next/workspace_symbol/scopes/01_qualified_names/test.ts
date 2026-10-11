import { answers } from "@clice/tools/testing";

answers({ paths: { server: "workspace symbols read the project index" } }, async (a) => {
    for (const query of ["net::Socket", "deep::Socket", "::net::Socket", "::deep::net::Socket"]) {
        await a.show(query, a.workspaceSymbols(query));
    }
});
