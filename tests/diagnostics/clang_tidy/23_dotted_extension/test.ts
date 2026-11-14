import { test } from "@clice/tools/testing";

// clang-tidy takes an extension without its dot: one written with it runs no
// check, and the editor and `clice lint` say which.
test("editor", async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});

test("lint", { errors: ["takes an extension without its dot"] }, async (t) => {
    await t.cli("lint --jobs 1");
});
