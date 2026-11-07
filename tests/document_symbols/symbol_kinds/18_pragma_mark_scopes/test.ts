import { answers } from "@clice/tools/testing";

answers(
    {
        indexDiffers:
            "the index records no inactive branch: until the AST is current, the mark in #if 0 lists and sections what follows it",
    },
    async (a) => {
        await a.show("outline", a.documentSymbols("main.cpp"), { parts: ["detail"] });
        await a.show("extents", a.documentSymbols("main.cpp"), { parts: ["range"] });
    },
);
