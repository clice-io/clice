import { test } from "@clice/tools/testing";

// The header instantiates show<app::Point>, whose call to describe picks,
// at the end of the file, the overload main.cpp deletes. The precompiled
// preamble instantiates it where the preamble ends, when only the header's
// overload exists, and the instantiation succeeds; clang compiling the
// whole file reports the call to the deleted function.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["related"] });
});
