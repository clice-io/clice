import { answers } from "@clice/tools/testing";

answers(
    {
        indexDiffers:
            "the index records no format strings: until the AST is current, each is one string with its escapes",
    },
    async (a) => {
        await a.show("main.cpp", a.semanticTokens("main.cpp"));
    },
);
