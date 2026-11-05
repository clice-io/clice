import { answers } from "@clice/tools/testing";

// The only instantiation's argument-dependent lookup finds N::g beside the
// visible ::g, which cannot take an N::S: the call names N::g.
answers(async (a) => {
    await a.show("main.cpp", a.inlayHints("main.cpp"));
});
