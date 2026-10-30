import { test } from "@clice/tools/testing";

// The project of a clice.toml that does not parse serves nothing: the
// undeclared name in main.cpp goes unreported.
test("nothing is served", { errors: ["Invalid clice\\.toml .*TOML parse error"] }, async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    await t.show("guidance", await s.guidance());
    await t.show("clice.toml", s.pushes("clice.toml"));
    await t.show("diagnostics", s.diagnostics("main.cpp"));
});
