import { test } from "@clice/tools/testing";

// `clice inspect hover_check` compiles each card's declaration again and
// compares it with the original: these forms come back the same.
test(async (t) => {
    await t.cli(["inspect", "hover_check", "main.cpp"]);
});
