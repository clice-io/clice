import { answers } from "@clice/tools/testing";

answers("keyword", (a) => a.show("keyword", a.completion(a.marker("keyword"))));

answers(
    "module names",
    { paths: { server: "clice inspect compiles one file alone, and main.cpp imports app" } },
    (a) => a.show("declared", a.completion(a.marker("declared"))),
);
