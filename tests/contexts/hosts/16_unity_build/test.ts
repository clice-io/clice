import { test, type TestContext } from "@clice/tools/testing";

// b.cpp calls a static function of a.cpp: it compiles only inside
// unity.cpp, which includes both.
async function answers(t: TestContext): Promise<void> {
    const s = await t.start();
    await s.indexed();
    // Asked once b.cpp compiled: until then the index answers.
    await s.errors("b.cpp");
    await t.show("b.cpp", {
        diagnostics: s.diagnostics("b.cpp"),
        contexts: s.contexts("b.cpp"),
        hover: s.hover(t.marker("call")),
        definition: s.definition(t.marker("call")),
        references: s.references(t.marker("helper")),
    });
}

// A source no database lists compiles in its includer's context.
test("unity source only", (t) => answers(t));

// A source the database lists is a unit of its own and compiles by its own
// command first; the unity context is there to switch to.
test("source listed too", async (t) => {
    t.disk.database({ "unity.cpp": [], "a.cpp": [], "b.cpp": [] });
    await answers(t);
});
