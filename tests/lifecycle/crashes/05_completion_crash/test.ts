import { test } from "@clice/tools/testing";

// A crash completing code pauses completion in the file; hover and the
// compile carry on.

const QUIET = {
    config: { diagnostics: { clang_tidy: false } },
    lastRule: { background_index: false },
};

test(async (t) => {
    const s = await t.start({ ...QUIET, crash: { request: "completion", file: "main.cpp" } });
    const add = t.marker("add");
    const complete = t.marker("complete");
    s.open("main.cpp");
    await t.show("hover", s.hover(add));
    await t.show("completion", s.completion(complete));
    await t.show("crashes", await s.crashes());
    await t.show("pushes", s.pushes("main.cpp"));
    await t.show("completion while paused", s.completion(complete));
    await t.show("hover while paused", s.hover(add));
    await t.show("crashes while paused", await s.crashes());
});
