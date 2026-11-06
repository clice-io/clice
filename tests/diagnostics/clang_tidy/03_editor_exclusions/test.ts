import { test } from "@clice/tools/testing";

// use-after-move is unreliable on code being edited and const-correctness
// too slow for an editor: of the three checks enabled, only integer division
// runs in the editor. The checks are named once for every file they apply to.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["link", "tags"] });
    await s.diagnostics("other.cpp");
    await t.show("guidance", await s.guidance());
});
