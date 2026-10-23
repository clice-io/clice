import { test } from "@clice/tools/testing";

// The header uses Counter<int>::value, whose definition main.cpp gives, in
// a function body the editor's compile skips. The background index compiles
// the whole file, instantiates the definition at the end and reports the
// error in its initializer.
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["related"] });
});
