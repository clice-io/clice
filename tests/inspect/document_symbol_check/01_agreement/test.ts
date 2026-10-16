import { test } from "@clice/tools/testing";

// `clice inspect document_symbol_check` sets the outline the index projects
// beside the compiled one: over these forms they list, name, nest and place
// every symbol alike.
test(async (t) => {
    await t.cli(["inspect", "document_symbol_check", "main.cpp"]);
});
