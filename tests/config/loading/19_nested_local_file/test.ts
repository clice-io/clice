import { test } from "@clice/tools/testing";

// The root's project would serve sub/main.cpp without the macro: sub/'s
// local file makes it a project of its own, which serves it with it.
test(async (t) => {
    const s = await t.start();
    await t.show("sub/main.cpp", s.diagnostics("sub/main.cpp"));
});
