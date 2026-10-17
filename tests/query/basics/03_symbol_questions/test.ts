import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index");
    const search = await t.cli("query symbol-search add");
    const [add] = (search.json as { result: { symbols: { symbol: string }[] } }).result.symbols;
    if (add === undefined) {
        throw new Error("symbol-search found no add");
    }
    await t.cli(["query", "definition", add.symbol]);
    await t.cli("query definition main.cpp:4");
    await t.cli("query read-symbol compute");
    await t.cli("query references add --include-declaration");
    await t.cli("query call-graph add --direction callers");
    await t.cli("query type-hierarchy Dog --direction supertypes");
    await t.cli("query implementation Animal");
    await t.cli("query document-symbols main.cpp");
    await t.cli("query symbol-search kind:Struct,Function --limit 10");
});
