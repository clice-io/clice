import { answers } from "@clice/tools/testing";

// A .clang-format with a misspelled key, which clang-format refuses: the
// editor's formatting should say so rather than do nothing.
answers({ paths: { server: "clice inspect does not format" } }, async (a) => {
    await a.show("main.cpp", a.formatting("main.cpp"));
});
