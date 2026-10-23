import { test } from "@clice/tools/testing";

// A g++ project's warning flags, four of them GCC's own, which g++ compiles
// main.cpp clean with. The compiler is one no machine has, so on every one
// clang's own driver reads the command, and the file says so.
test({ errors: ["Toolchain query failed"] }, async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
