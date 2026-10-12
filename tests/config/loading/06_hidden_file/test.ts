import { test } from "@clice/tools/testing";

// Without a clice.toml, the configuration is read from .clice/config.toml.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
