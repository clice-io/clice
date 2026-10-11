import { answers, test } from "@clice/tools/testing";

// main.cpp hosts widget.h. Widget::b is defined in other.cpp, which main.cpp
// never sees: only the index drops it from what the host receives, and the
// rest goes after main.cpp's definition of Widget::a.
answers(
    "index",
    {
        indexing: true,
        paths: { server: "only the server builds the index and resolves the host source" },
    },
    async (a) => {
        await a.show("cls", a.codeActions(a.marker("cls"), { compile: true }));
    },
);

// The answers test's compile check reads the requested header's diagnostics
// alone; this applies the host action and reads the host's own.
test("host compiles", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: true } } });
    await s.diagnostics("main.cpp");
    await s.diagnostics("widget.h");
    await s.indexed();
    await t.show(
        "widget.h",
        s.applyAction(t.marker("cls"), "Define missing members of 'Widget' in main.cpp"),
    );
    await t.show("main.cpp", s.errors("main.cpp"));
});
