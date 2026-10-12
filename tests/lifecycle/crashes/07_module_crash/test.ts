import { test } from "@clice/tools/testing";

// A crash building a module is noted on its importers. The importer still
// compiles, reporting the missing module; the module is built again once an
// importer changes or is saved, or the module itself changes, and another
// importer learns the crash without one of its own.

const QUIET = { diagnostics: { clang_tidy: false }, project: { enable_indexing: false } };

test(async (t) => {
    const s = await t.start({ config: QUIET, crash: { request: "buildPcm", file: "math.cppm" } });
    const call = t.at("main.cpp", "return |add(1, 2)");
    s.open("main.cpp");
    await t.show("hover", s.hover(call));
    await t.show("crashes", await s.crashes());
    await t.show("pushes", s.pushes("main.cpp"));
    await t.show("hover asked again", s.hover(call));
    await t.show("crashes while it sits still", await s.crashes());
    s.save("main.cpp");
    await t.show("hover after a save", s.hover(call));
    await t.show("crashes after a save", await s.crashes());
    const other = t.at("other.cpp", "return |add(3, 4)");
    s.open("other.cpp");
    await t.show("other hover", s.hover(other));
    await t.show("other pushes", s.pushes("other.cpp"));
    await t.show("crashes after the other", await s.crashes());
    t.disk.edit("math.cppm", { after: "}\n", insert: "// edited\n" });
    await s.sync({ poll: true });
    await t.show("other hover after the module changed", s.hover(other));
    await t.show("crashes after the module changed", await s.crashes());
});
