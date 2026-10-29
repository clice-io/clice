import { test } from "@clice/tools/testing";

// A crash building a module is known to have read the modules it imports:
// a change to one of them builds it again, with no change to its own text.

const QUIET = { diagnostics: { clang_tidy: false }, project: { enable_indexing: false } };

test(async (t) => {
    const s = await t.start({ config: QUIET, crash: { request: "buildPcm", file: "math.cppm" } });
    const call = t.at("main.cpp", "return |add(1, 2)");
    s.open("main.cpp");
    await t.show("hover", s.hover(call));
    await t.show("crashes", await s.crashes());
    await t.elapse(2_100, "the spacing between a crash and the retry a change earns");
    t.disk.edit("base.cppm", { after: "}\n", insert: "// edited\n" });
    await s.sync({ poll: true });
    await t.show("hover after the import changed", s.hover(call));
    await t.show("crashes after the import changed", await s.crashes());
});
