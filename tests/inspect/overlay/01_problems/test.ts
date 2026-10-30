import { test } from "@clice/tools/testing";

// An overlay naming a section the configuration lacks is a typo: a warning,
// and the rest applies.
test("an unknown section", async (t) => {
    await t.cli([
        "inspect",
        "semantic-tokens",
        "main.cpp",
        "--config-overlay",
        '{"no_such_section": {}}',
    ]);
});

// One that is no JSON, or gives a value of the wrong type, stops the run.
test("not JSON", async (t) => {
    await t.cli(["inspect", "semantic-tokens", "main.cpp", "--config-overlay", "{not json"]);
});

test("a wrong type", async (t) => {
    await t.cli([
        "inspect",
        "semantic-tokens",
        "main.cpp",
        "--config-overlay",
        '{"project": {"enable_indexing": "yes"}}',
    ]);
});
