import { test } from "@clice/tools/testing";

// A compiler no machine has, for a target clang does not know: clang's own
// driver cannot read the command in its place either, and the compile
// reports clang's error alone.
test({ errors: ["Toolchain query failed"] }, async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
