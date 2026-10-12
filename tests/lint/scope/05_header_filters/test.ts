import { test } from "@clice/tools/testing";

// Pins the header filters: by default every header but a system one reports;
// HeaderFilterRegex, ExcludeHeaderFilterRegex and SystemHeaders narrow and
// widen that as in clang-tidy, matched against the path the include found,
// spelled with the host's separators.
test("defaults", async (t) => {
    await t.cli("lint --workers 2");
});

test("configured", async (t) => {
    t.disk.write(
        ".clang-tidy",
        [
            'Checks: "-*,bugprone-integer-division"',
            "HeaderFilterRegex: 'include[/\\\\]|system[/\\\\]'",
            "ExcludeHeaderFilterRegex: 'detail[/\\\\]'",
            "SystemHeaders: true",
            "",
        ].join("\n"),
    );
    await t.cli("lint --workers 2");
});
