import { answers } from "@clice/tools/testing";

answers(async (a) => {
    await a.show("main.cpp", a.documentLinks("main.cpp"));
    await a.show("first/wrap.h", a.documentLinks("first/wrap.h"));
});
