import { test } from "@clice/tools/testing";

const QUIET = { diagnostics: { clang_tidy: false }, project: { enable_indexing: false } };

// A change to a header the preamble includes is a retry, with no save of
// the file itself.
test("a header change retries", async (t) => {
    const s = await t.start({ config: QUIET, crash: { request: "buildPch", file: "main.cpp" } });
    const add = t.marker("add");
    s.open("main.cpp");
    await t.show("hover", s.hover(add));
    await t.show("crashes", await s.crashes());
    await t.show("pushes", s.pushes("main.cpp"));
    await t.show("hover asked again", s.hover(add));
    await t.show("crashes while it sits still", await s.crashes());
    await t.elapse(2_100, "the spacing between a crash and the retry a change earns");
    t.disk.edit("header.h", { after: "int known();\n", insert: "int more();\n" });
    await s.sync({ poll: true });
    await t.show("hover after the header changed", s.hover(add));
    await t.show("crashes after the header changed", await s.crashes());
});

// The first crash compiling against a preamble may be the preamble's own
// corruption: the pair is rebuilt and the compile run once more, and only
// that second crash is the file's.
test("a crash reading the preamble rebuilds it", async (t) => {
    const s = await t.start({ config: QUIET, crash: { request: "compile", file: "main.cpp" } });
    s.open("main.cpp");
    await t.show("hover", s.hover(t.marker("add")));
    await t.show("crashes", await s.crashes());
    await t.show("pushes", s.pushes("main.cpp"));
    await t.show("builds", { pch: (await s.counts()).pch });
});
