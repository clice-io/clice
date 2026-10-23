import { test } from "@clice/tools/testing";

// A completion parses up to its point: a directive typed at the end of the
// preamble keeps the PCH of the lines above it, and the one the edited
// preamble needs, held in its worker here, is not waited for.
test("a directive typed after the includes", async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    const park = await s.park("buildPch", "main.cpp", { at: "worker" });
    s.edit("main.cpp", { after: '#include "b.h"\n', insert: "#ifdef DIR_\n#endif\n" });
    await t.show("macros", s.completion(t.at("main.cpp", "#ifdef DIR_|")));
    await park.release();
});

// The PCH a completion between the includes builds is its own: the
// document keeps the one its compile built, whose names complete there.
test("a directive typed between the includes", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await s.diagnostics("main.cpp");
    s.edit("main.cpp", { after: '#include "a.h"\n', insert: "#ifdef DIR_\n#endif\n" });
    await s.diagnostics("main.cpp");
    await t.show("macros", s.completion(t.at("main.cpp", "#ifdef DIR_|")));
    await t.show("names", s.completion(t.at("main.cpp", "beta_|")));
});
