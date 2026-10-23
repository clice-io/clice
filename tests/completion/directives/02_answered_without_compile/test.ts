import { test } from "@clice/tools/testing";

// The directives are a fixed list: no compile answers their names, so a
// completion held in its worker holds none of them.
test("directive names", async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    const park = await s.park("completion", "main.cpp", { at: "worker" });
    await t.show("include", s.completion(t.marker("include")));
    await park.release();
});
