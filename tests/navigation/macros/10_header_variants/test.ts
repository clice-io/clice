import { test } from "@clice/tools/testing";

// The header stays closed: the index holds one variant of it per unit, each
// expanding its own definition of the macro.
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("use", { definition: s.definition(t.marker("use")) });
});
