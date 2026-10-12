import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index");
    const search = await t.cli("query --method symbolSearch --symbol add");
    const [add] = (search.json as { result: { symbols: { symbol: string }[] } }).result.symbols;
    if (add === undefined) {
        throw new Error("symbolSearch found no add");
    }
    await t.cli(["query", "--method", "definition", "--symbol", add.symbol]);
    await t.cli("query --method definition --symbol main.cpp:4");
    await t.cli("query --method readSymbol --symbol compute");
    await t.cli("query --method references --symbol add --include-declaration");
    await t.cli("query --method callGraph --symbol add --direction callers");
    await t.cli("query --method typeHierarchy --symbol Dog --direction supertypes");
    await t.cli("query --method documentSymbols --path main.cpp");
    await t.cli("query --method symbolSearch --symbol kind:Struct,Function --limit 10");
});
