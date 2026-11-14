import { test } from "@clice/tools/testing";

// Without a clice.toml, the configuration is read from .clice/config.toml,
// whose relative patterns start at the project root, as clice.toml's do:
// "main.cpp" names the file at the root.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
