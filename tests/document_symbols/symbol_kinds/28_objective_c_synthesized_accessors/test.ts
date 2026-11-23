import { answers } from "@clice/tools/testing";

answers(async (a) => {
    await a.show("outline", a.documentSymbols("main.m"), { parts: ["detail"] });
    await a.show("extents", a.documentSymbols("main.m"), { parts: ["range"] });
});
