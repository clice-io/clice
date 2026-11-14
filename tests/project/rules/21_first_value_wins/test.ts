import { test } from "@clice/tools/testing";

// Of the rules matching a file, the first that sets a field decides it: the
// rule for keep/ goes before the rule for every file, which keeps the rest
// out of the index.
test("a narrow rule before a broad one", async (t) => {
    await t.cli("index");
    await t.cli("query symbol-search kept");
    await t.cli("query symbol-search skipped");
});

// The other way round, the rule for every file decides keep/ too, and the
// narrow rule after it is a warning: its value never applies.
test("a broad rule before a narrow one", async (t) => {
    t.disk.write(
        "clice.toml",
        '[[rules]]\nbackground_index = false\n\n[[rules]]\npatterns = ["keep/**"]\nbackground_index = true\n',
    );
    await t.cli("index");
});
