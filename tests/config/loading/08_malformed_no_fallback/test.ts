import { test } from "@clice/tools/testing";

// A clice.toml that does not parse is the project's configuration all the
// same: .clice/config.toml does not stand in, and nothing is served.
test({ errors: ["Invalid clice\\.toml .*TOML parse error"] }, async (t) => {
    const s = await t.start();
    await t.show("clice.toml", s.pushes("clice.toml"));
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
