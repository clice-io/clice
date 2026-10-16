import { test } from "@clice/tools/testing";

// Pins that a suppression comment silences a compiler warning when the
// checks enable no clang-tidy check of their own, only warnings.
test(async (t) => {
    await t.cli("lint --jobs 2");
});
