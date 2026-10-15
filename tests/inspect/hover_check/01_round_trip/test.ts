import { test } from "@clice/tools/testing";

// `clice inspect hover_check` compiles each card's declaration again and
// compares it with the original: these forms come back the same, and an
// instantiation's member whose initializer clang has not instantiated yet
// comes back without one.
test(async (t) => {
    await t.cli(["inspect", "hover_check", "main.cpp"]);
    await t.cli(["inspect", "hover_check", "limit.cpp"]);
});
