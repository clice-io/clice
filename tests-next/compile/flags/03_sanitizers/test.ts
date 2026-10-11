import { answers } from "@clice/tools/testing";

// The driver adds sanitizer files of its own resource directory, which
// clice's does not carry: both units must still compile clean.
answers(async (a) => {
    await a.show("main.cpp", a.documentLinks("main.cpp"));
    await a.show("sub/main.cpp", a.documentLinks("sub/main.cpp"));
});
