import { test } from "@clice/tools/testing";

// A cl no machine has: the toolchain query fails on every one, and the
// command is read without it all the same.
test({ errors: ["Toolchain query failed: Cannot find driver: cl-missing\\.exe"] }, async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
