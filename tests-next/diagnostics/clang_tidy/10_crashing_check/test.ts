import { test } from "@clice/tools/testing";

// clang-tidy crashes over this file: the compile runs again without it, so
// the file keeps its features and compiler diagnostics, and only the
// findings pause.
test(async (t) => {
    const s = await t.start({
        crash: { request: "compile", file: "main.cpp", tidy: "tidy poison" },
    });
    await t.show("main.cpp", s.diagnostics("main.cpp"));
    await t.show("hover", s.hover(t.at("main.cpp", "double |ratio")));
    await t.show("crashes", await s.crashes());
});
