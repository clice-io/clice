import { test } from "@clice/tools/testing";

// A clice.toml that does not parse gives way to .clice/config.toml, and
// still gets its error.
test({ errors: ["Invalid clice\\.toml .*TOML parse error"] }, async (t) => {
    const s = await t.start();
    await t.show("clice.toml", s.pushes("clice.toml"));
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
