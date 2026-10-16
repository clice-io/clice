import { test } from "@clice/tools/testing";

// A .clang-tidy that does not parse runs no check, as `clice lint` refuses
// the file, and the file says why at its top.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
