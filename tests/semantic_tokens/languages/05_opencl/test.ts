import { answers } from "@clice/tools/testing";

answers(async (a) => {
    await a.show("main.cl", a.semanticTokens("main.cl"));
    await a.show("main.clcpp", a.semanticTokens("main.clcpp"));
});
