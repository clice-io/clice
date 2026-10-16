import { test } from "@clice/tools/testing";

// Pins the checks that run besides those a .clang-tidy names, as clang-tidy
// runs them: its checks start from clang-diagnostic-*, the compiler warnings
// of the build, which are all that run without one.

test("without a configuration", async (t) => {
    t.disk.rm(".clang-tidy");
    await t.cli("lint --jobs 2");
});

test("checks without -*", async (t) => {
    await t.cli("lint --jobs 2");
});
