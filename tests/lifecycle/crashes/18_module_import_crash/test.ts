import { test } from "@clice/tools/testing";

// A crash building a module is known to have read the modules it imports:
// once one of them changes, an importer with no crash of its own builds the
// module again instead of learning the old crash.

const QUIET = { diagnostics: { clang_tidy: false }, project: { enable_indexing: false } };

test(async (t) => {
    const s = await t.start({ config: QUIET, crash: { request: "buildPcm", file: "math.cppm" } });
    s.open("main.cpp");
    await t.show("hover", s.hover(t.at("main.cpp", "return |add(1, 2)")));
    await t.show("crashes", await s.crashes());
    s.close("main.cpp");
    t.disk.edit("base.cppm", { after: "}\n", insert: "// edited\n" });
    await s.sync({ poll: true });
    s.open("other.cpp");
    await t.show("other hover", s.hover(t.at("other.cpp", "return |add(3, 4)")));
    await t.show("crashes after the import changed", await s.crashes());
});
