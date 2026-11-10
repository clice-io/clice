import { test } from "@clice/tools/testing";

// The toolchain's directories, its builtin headers among them, join the
// search once its query has answered: the compile awaited first asks it.
test("builtin headers", async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    await t.show("builtin", s.completion(t.marker("builtin")), { parts: ["insert", "edit"] });
});
