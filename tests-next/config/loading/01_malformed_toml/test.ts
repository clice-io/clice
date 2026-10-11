import { test } from "@clice/tools/testing";

test("defaults stand in", { errors: ["Invalid clice\\.toml .*TOML parse error"] }, async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    await t.show("guidance", await s.guidance());
    await t.show("diagnostics", s.diagnostics("main.cpp"));
});
