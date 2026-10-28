import { answers } from "@clice/tools/testing";

answers(
    {
        paths: {
            server: "clice inspect compiles one file alone, and main.cpp imports its partitions",
        },
    },
    (a) => a.show("main.cpp", a.foldingRanges("main.cpp")),
);
