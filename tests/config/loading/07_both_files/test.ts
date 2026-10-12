import { test } from "@clice/tools/testing";

// clice.toml is read, and .clice/config.toml beside it is not.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
