import { test } from "@clice/tools/testing";

// The header instantiates less<int> through check<int>, and the precompiled
// preamble instantiates it there, under the header's request. clang
// compiling the file as a whole instantiates it at the end, under the
// main file's request, and reports the warning on that call.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["related"] });
});
