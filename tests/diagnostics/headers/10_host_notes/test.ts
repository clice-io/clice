import { test } from "@clice/tools/testing";

// rules.h compiles inside host.cpp's function: the notes of its errors point
// into host.cpp, before the function and inside it.
test(async (t) => {
    const s = await t.start();
    await t.show("rules.h", s.diagnostics("rules.h"), { parts: ["related"] });
});
