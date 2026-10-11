import { answers } from "@clice/tools/testing";

answers(
    {
        paths: {
            separate:
                "the server compiles the leading directives into a PCH, whose defines have no nodes in the main parse",
        },
    },
    (a) => a.show("main.cpp", a.semanticTokens("main.cpp")),
);
