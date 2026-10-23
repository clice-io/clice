import { test } from "@clice/tools/testing";

// A compiler no machine has, for a target clang does not know: clang's own
// driver reads the command in its place, which the file says, and the
// compile fails on clang's error.
test({ errors: ["Toolchain query failed"] }, async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
