import { test } from "@clice/tools/testing";

// A module whose own import names no module breaks its importers' import
// too.
test(async (t) => {
    const s = await t.start();
    await t.show("b.cppm", s.errors("b.cppm"));
    await t.show("a.cppm", s.errors("a.cppm"));
});
