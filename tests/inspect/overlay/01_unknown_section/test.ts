import { test } from "@clice/tools/testing";

// An overlay naming a section the configuration lacks is a typo, refused
// rather than ignored.
test(async (t) => {
    await t.cli([
        "inspect",
        "semantic-tokens",
        "main.cpp",
        "--config-overlay",
        '{"no_such_section": {}}',
    ]);
});
