import { test } from "@clice/tools/testing";

// The header instantiates show<app::Point>, whose call to describe picks,
// at the end of the file, the overload main.cpp deletes. The editor's
// compile skips the header's function bodies; the background index
// compiles the whole file and reports the call to the deleted function.
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["related"] });
});
