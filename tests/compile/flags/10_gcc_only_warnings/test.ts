import { test } from "@clice/tools/testing";

// A g++ project's warning flags, four of them GCC's own; g++ compiles
// main.cpp clean. Where no g++ is installed the toolchain query fails and
// clang's own driver reads the command, which a warning on the file says on
// those machines alone.
test({ errors: ["Toolchain query failed"] }, async (t) => {
    const s = await t.start();
    const diagnostics = await s.diagnostics("main.cpp");
    await t.show(
        "main.cpp",
        diagnostics.filter((diagnostic) => diagnostic.code !== "toolchain-query-failed"),
    );
});
