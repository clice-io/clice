import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start();
    await t.show("main.m", s.diagnostics("main.m"));
    await t.show("guidance", await s.guidance());
});
