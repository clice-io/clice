import { test } from "@clice/tools/testing";

// The compile crashes, and again without clang-tidy: the second crash
// acquits it, and the retry a save grants runs without clang-tidy too. The
// index stays off, whose entries would answer what the crashed compile cannot
// and whose own run of the file would crash as well.
test(async (t) => {
    const s = await t.start({
        config: { project: { enable_indexing: false } },
        crash: { request: "compile", file: "main.cpp", pragma: true },
    });
    const add = t.at("main.cpp", "int |add");
    s.open("main.cpp");
    await t.show("hover", s.hover(add));
    await t.show("main.cpp", s.diagnostics("main.cpp"));
    await t.show("crashes", await s.crashes());
    s.save("main.cpp");
    await t.show("hover after a save", s.hover(add));
    await t.show("crashes after a save", await s.crashes());
});
