import { test } from "@clice/tools/testing";

// The importer's index is on disk, and a rule keeps the interface its name
// comes from out of the background index: the build of its PCM indexes it
// all the same. The importer's compile is held, so the requests land before
// it as they do when a user is quick, and find the declaration in that index.
test(async (t) => {
    const s = await t.start({ config: { rules: [{ patterns: ["math.cppm"], index: false }] } });
    await s.indexed();
    const compile = await s.park("compile", "main.cpp", { at: "worker" });
    s.open("main.cpp");
    await compile.reached();
    await t.show("compiling", {
        declaration: s.declaration(t.marker("use")),
        references: s.references(t.marker("use")),
    });
    await compile.release();
    await s.sync();
    await t.show("compiled", {
        definition: s.definition(t.marker("use")),
        declaration: s.declaration(t.marker("use")),
        references: s.references(t.marker("use")),
    });
});
