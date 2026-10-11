import { answers } from "@clice/tools/testing";

answers(
    { paths: { server: "clice inspect compiles one file alone, and main.cppm imports modules" } },
    (a) => a.show("main.cppm", a.documentLinks("main.cppm")),
);
