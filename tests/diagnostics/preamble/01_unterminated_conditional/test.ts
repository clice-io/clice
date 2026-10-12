import { test } from "@clice/tools/testing";

// The #ifdef is never closed and sits among the leading directives the
// precompiled preamble holds: the error stays on it.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
