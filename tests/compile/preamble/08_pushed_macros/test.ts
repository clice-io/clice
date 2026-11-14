import { test } from "@clice/tools/testing";

// main.cpp pushes min and LIMIT before its include, which goes into the
// precompiled preamble, and pops them in the body, the way Windows code
// steps around the min/max macros. It compiles clean under clang.
test("pushed before the include", async (t) => {
    const s = await t.start({ lastRule: { background_index: false } });
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
