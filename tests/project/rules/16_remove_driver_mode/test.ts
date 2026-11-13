import { test } from "@clice/tools/testing";

// The rule takes cl mode out of the command: main.cpp compiles for the
// pinned target, where _MSC_VER is not defined.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.errors("main.cpp"));
});
