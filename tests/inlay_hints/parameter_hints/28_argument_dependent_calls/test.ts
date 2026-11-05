import { answers } from "@clice/tools/testing";

// The only instantiation's argument-dependent lookup finds N::g beside the
// visible ::g: which one the call takes is overload resolution's, so no
// parameter names.
answers(async (a) => {
    await a.show("main.cpp", a.inlayHints("main.cpp"));
});
