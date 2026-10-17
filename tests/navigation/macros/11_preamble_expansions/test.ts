import { test } from "@clice/tools/testing";

// No background index: the headers answer from the open file's preamble.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    s.open("main.cpp");
    await s.sync();
    await t.show("reset", { references: s.references(t.marker("reset")) });
});
