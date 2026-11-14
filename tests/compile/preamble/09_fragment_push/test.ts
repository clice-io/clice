import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start({ lastRule: { background_index: false } });
    await t.show("m.cppm", s.diagnostics("m.cppm"));
});
