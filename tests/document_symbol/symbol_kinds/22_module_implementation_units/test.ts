import { answers } from "@clice/tools/testing";

// A module implementation unit lists the module it belongs to.
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
