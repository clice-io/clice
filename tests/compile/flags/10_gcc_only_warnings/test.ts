import { test } from "@clice/tools/testing";

// A g++ project's warning flags, four of them GCC's own; g++ compiles
// main.cpp clean. Where no g++ is installed the toolchain query fails and
// clice's own driver reads the command.
test({ errors: ["Toolchain query failed"] }, async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
