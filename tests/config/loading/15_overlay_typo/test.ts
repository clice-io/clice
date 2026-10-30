import { test } from "@clice/tools/testing";

// --config-overlay misspells a key: a warning, and the rest applies.
test(async (t) => {
    await t.cli([
        "inspect",
        "document-symbols",
        "main.cpp",
        "--config-overlay",
        '{"diagnostics": {"clang_tdy": false}}',
    ]);
});
