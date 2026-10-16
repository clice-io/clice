import { answers } from "@clice/tools/testing";

answers(
    {
        paths: {
            server: "clice inspect compiles one file alone, and an implementation unit imports its interface",
        },
    },
    async (a) => {
        await a.show("outline", a.documentSymbols("main.cpp"), { parts: ["detail"] });
    },
);
