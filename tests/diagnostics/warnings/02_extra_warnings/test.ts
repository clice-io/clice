import { test } from "@clice/tools/testing";

// -Wextra and -Wconversion also name flang options that flang ignores; the
// command keeps them as clang's warning options.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
