import { test } from "@clice/tools/testing";

// Half-typed code crashing the compiler is the common case while editing:
// the first crash stays silent, each later edit earns one spaced retry, and
// after three strikes in a row only a save retries.

const QUIET = { diagnostics: { clang_tidy: false }, project: { enable_indexing: false } };

test(async (t) => {
    const s = await t.start({
        config: QUIET,
        crash: { request: "compile", file: "main.cpp", pragma: true },
    });
    const add = t.at("main.cpp", "int |add");
    s.open("main.cpp");
    await t.show("diagnostics", s.diagnostics("main.cpp"));
    s.edit("main.cpp", { after: "}\n", insert: "// edit 1\n#pragma clang __debug crash\n" });
    await t.show("hover after edit 1", s.hover(add));
    await t.show("crashes after edit 1", await s.crashes());
    for (const edit of [2, 3, 4]) {
        await t.elapse(2_100, "the spacing between a crash and the retry an edit earns");
        s.edit("main.cpp", { before: "#pragma", insert: `// edit ${String(edit)}\n` });
        await t.show(`hover after edit ${String(edit)}`, s.hover(add));
        await t.show(`crashes after edit ${String(edit)}`, await s.crashes());
    }
    await t.show("pushes", s.pushes("main.cpp"));
    s.save("main.cpp");
    await t.show("hover after a save", s.hover(add));
    await t.show("crashes after a save", await s.crashes());
});
