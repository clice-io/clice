import { test } from "@clice/tools/testing";

// A file's outline from the persisted index: what it declares and
// defines, where each starts and ends.
test(async (t) => {
    await t.cli("index");
    await t.cli("query --method documentSymbols --path main.cpp");
    await t.cli("query --method documentSymbols --path api.h");
    await t.cli("query --method readSymbol --symbol twice");
});
