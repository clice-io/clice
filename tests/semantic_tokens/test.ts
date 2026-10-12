import { answers } from "@clice/tools/testing";

answers(async (a) => {
    const markers = a.markers();
    if (markers.length === 0) {
        await a.show("main.cpp", a.semanticTokens("main.cpp"));
    }
    for (const m of markers) {
        await a.show(m.name, a.semanticTokens(m));
    }
});
