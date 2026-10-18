import { test } from "@clice/tools/testing";

// clang-tidy's crash is retried without it, and the index's entries of the
// disk text would answer what the crashed compile cannot: both stay off, so
// every crash here is the compile's own.
const QUIET = { diagnostics: { clang_tidy: false }, project: { enable_indexing: false } };
const CRASH = { request: "compile", file: "main.cpp", pragma: true } as const;

test("a save is the retry", async (t) => {
    const s = await t.start({ config: QUIET, crash: CRASH });
    const add = t.marker("add");
    s.open("main.cpp");
    await t.show("hover", s.hover(add));
    await t.show("pushes", s.pushes("main.cpp"));
    await t.show("crashes", await s.crashes());
    await t.show("hover asked again", s.hover(add));
    await t.show("symbols", s.documentSymbols("main.cpp"));
    await t.show("crashes while it sits still", await s.crashes());
    s.save("main.cpp");
    await t.show("hover after a save", s.hover(add));
    await t.show("crashes after a save", await s.crashes());
});

test("a pull shows the note", async (t) => {
    const s = await t.start({
        config: QUIET,
        crash: CRASH,
        capabilities: { textDocument: { diagnostic: {} } },
    });
    await t.show("pulled", s.diagnostics("main.cpp"));
    await t.show("crashes", await s.crashes());
    await t.show("pushes", s.pushes("main.cpp"));
});

test("a fix retries after a pause", async (t) => {
    const s = await t.start({ config: QUIET, crash: CRASH });
    s.open("main.cpp");
    await t.show("hover", s.hover(t.marker("add")));
    await t.show("crashes", await s.crashes());
    s.edit("main.cpp", { remove: "#pragma clang __debug crash\n" });
    await t.elapse(2_100, "the spacing between a crash and the retry an edit earns");
    await t.show("hover after the fix", s.hover(t.at("main.cpp", "int |add")));
    await t.show("pushes", s.pushes("main.cpp"));
    await t.show("crashes after the fix", await s.crashes());
});

test("reopening keeps the bar", async (t) => {
    const s = await t.start({ config: QUIET, crash: CRASH });
    const add = t.marker("add");
    s.open("main.cpp");
    await t.show("hover", s.hover(add));
    await t.show("crashes", await s.crashes());
    s.close("main.cpp");
    s.open("main.cpp");
    await t.show("hover after a reopen", s.hover(add));
    await t.show("pushes", s.pushes("main.cpp"));
    await t.show("crashes after a reopen", await s.crashes());
    s.save("main.cpp");
    await t.show("hover after a save", s.hover(add));
    await t.show("crashes after a save", await s.crashes());
});
