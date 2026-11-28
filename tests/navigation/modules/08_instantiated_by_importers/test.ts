import { test } from "@clice/tools/testing";

// The interface instantiates nothing: what the importer's instantiations
// resolve there reaches it through the index. `extern "C++"` attaches the
// template to the global module, its text the interface's all the same.
test("instantiated by an importer", async (t) => {
    const s = await t.start();
    await s.indexed();
    s.open("main.cppm");
    await s.sync();
    for (const m of t.markers()) {
        await t.show(m.name, { definition: s.definition(m) });
    }
});
