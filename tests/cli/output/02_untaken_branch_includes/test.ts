import { test } from "@clice/tools/testing";

// main.cpp compiles cleanly: each include it names sits in a branch the
// preprocessor never takes.
test(async (t) => {
    await t.cli("index");
    await t.cli("lint");
});
