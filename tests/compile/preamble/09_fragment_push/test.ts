import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("m.cppm", s.diagnostics("m.cppm"));
});
