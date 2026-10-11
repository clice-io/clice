import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not format" } }, async (a) => {
    await a.show("main.cpp", a.formatting("main.cpp"));
});
