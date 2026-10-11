import { test } from "@clice/tools/testing";

// A static function in a header is a different symbol in every source
// including it, and a local variable lives in one function: references
// reach them all the same, and their locators name a file holding their
// rows, any file seeing them, workspace-relative or absolute.
test(async (t) => {
    await t.cli("index");
    await t.cli("query --method references --symbol util.h:2:12 --include-declaration");
    await t.cli("query --method references --symbol a.cpp:2:18 --include-declaration");
    await t.cli("query --method references --symbol b.cpp:2:15 --include-declaration");
    const outline = await t.cli("query --method documentSymbols --path util.h");
    await t.cli("query --method documentSymbols --path b.cpp");
    const [listed] = (outline.json as { result: { symbols: { symbol: string }[] } }).result.symbols;
    if (listed === undefined) {
        throw new Error("the outline of util.h lists nothing");
    }
    const id = listed.symbol.split(" ")[0] ?? "";
    for (const locator of [listed.symbol, `${id} path:util.h`, `${id} path:a.cpp`]) {
        await t.cli(["query", "--method", "definition", "--symbol", locator]);
    }
    await t.cli(["query", "--method", "symbolSearch", "--symbol", `${id} path:a.cpp`]);
    await t.cli(["query", "--method", "definition", "--symbol", `${id} path:gone.cpp`]);
    t.disk.write("new.cpp", "int fresh() { return 1; }\n");
    await t.cli(["query", "--method", "definition", "--symbol", `${id} path:new.cpp`]);
    // By name, a file's own symbol is found where it is declared.
    await t.cli(["query", "--method", "symbolSearch", "--symbol", '"helper" path:util.h']);
    await t.cli(["query", "--method", "symbolSearch", "--symbol", '"helper" path:a.cpp']);
});
