import { test } from "@clice/tools/testing";

// clang++ accepts this command and compiles main.cpp clean.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
