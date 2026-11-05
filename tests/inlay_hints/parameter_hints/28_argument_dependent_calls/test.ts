import { answers } from "@clice/tools/testing";

// The only instantiation's argument-dependent lookup finds N::g beside the
// visible ::g; the resolver does not tell them apart by argument types.
answers(async (a) => {
    await a.show("main.cpp", a.inlayHints("main.cpp"));
});
