import { test } from "@clice/tools/testing";

// Where cl.exe is not on the path the toolchain query fails, and the command
// is read without it all the same.
test({ errors: ["Toolchain query failed: Cannot find driver: cl\\.exe"] }, async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
