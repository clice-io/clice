import { test } from "@clice/tools/testing";

// `clice inspect document_symbol_check` sets the outline the index projects
// beside the compiled one: over these forms they list, name, nest and place
// every symbol alike. The index knows no inactive branch, so a mark in one
// sections what follows it in the index's outline alone.
test(async (t) => {
    await t.cli(["inspect", "document_symbol_check", "main.cpp"]);
    await t.cli(["inspect", "document_symbol_check", "marks.cpp"]);
});
