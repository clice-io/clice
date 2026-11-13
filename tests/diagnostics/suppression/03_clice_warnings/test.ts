import { test } from "@clice/tools/testing";

// clice's own warnings are suppressed by their code as the compiler's are.
test("shown", async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});

test("suppressed", async (t) => {
    const s = await t.start({ config: { diagnostics: { suppress: ["toolchain-query-failed"] } } });
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
