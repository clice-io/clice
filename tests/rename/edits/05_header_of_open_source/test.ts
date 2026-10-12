import { test } from "@clice/tools/testing";

// main.cpp opens before the background index reaches it, and nothing else
// includes the header.
test(async (t) => {
    const s = await t.spawn();
    s.open("main.cpp", { pull: false });
    await s.initialize();
    await s.indexed();
    await t.show("measure", s.rename(t.marker("measure"), "twice"));
});
