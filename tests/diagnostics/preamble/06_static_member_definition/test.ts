import { test } from "@clice/tools/testing";

// The header uses Counter<int>::value, whose definition main.cpp gives. The
// precompiled preamble instantiates it where the preamble ends, before the
// definition, and drops it; clang compiling the whole file instantiates the
// definition at the end and reports the error in its initializer.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["related"] });
});
