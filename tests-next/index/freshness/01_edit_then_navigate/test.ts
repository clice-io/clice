import { test } from "@clice/tools/testing";

// Navigation right after an edit resolves against the edited buffer: the
// line inserted on top shifts every position, so an answer from the index
// of the text before would name the wrong place or nothing. The requests
// are sent with no wait after the edit.
test(async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    s.edit("main.cpp", { before: "int foo()", insert: "// shift\n" });
    const call = t.at("main.cpp", "return |foo()");
    await t.show("after the edit", {
        definition: s.definition(call),
        uses: s.references(call, { declarations: false }),
    });
});
