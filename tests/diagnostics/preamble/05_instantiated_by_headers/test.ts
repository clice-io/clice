import { test } from "@clice/tools/testing";

// The header instantiates less<int> through check<int>. The editor's
// compile of main.cpp skips the header's function bodies; the background
// index compiles the file as a whole, which instantiates less<int> at the
// end, under main.cpp's request, and reports the warning on that call.
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["related"] });
});
