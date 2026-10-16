import { test } from "@clice/tools/testing";

// `clice inspect document-symbols --check` sets the outline the index projects
// beside the compiled one: over these forms they list, name, nest and place
// every symbol alike. The index knows no inactive branch, so a mark in one
// sections what follows it in the index's outline alone.
test(async (t) => {
    await t.cli(["inspect", "document-symbols", "main.cpp", "--check"]);
    await t.cli(["inspect", "document-symbols", "marks.cpp", "--check"]);
});
