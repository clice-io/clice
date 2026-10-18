import { test } from "@clice/tools/testing";

// A static function in a header is a different symbol in every source
// including it, and a local variable lives in one function: references
// reach them all the same, and their locators name a file holding their
// entries, any file seeing them, workspace-relative or absolute.
test(async (t) => {
    await t.cli("index");
    await t.cli("query references util.h:2:12 --include-declaration");
    await t.cli("query references a.cpp:2:18 --include-declaration");
    await t.cli("query references b.cpp:2:15 --include-declaration");
    const outline = await t.cli("query document-symbols util.h");
    await t.cli("query document-symbols b.cpp");
    const [listed] = (outline.json as { result: { symbols: { symbol: string }[] } }).result.symbols;
    if (listed === undefined) {
        throw new Error("the outline of util.h lists nothing");
    }
    const id = listed.symbol.split(" ")[0] ?? "";
    for (const locator of [listed.symbol, `${id} path:util.h`, `${id} path:a.cpp`]) {
        await t.cli(["query", "definition", locator]);
    }
    await t.cli(["query", "symbol-search", `${id} path:a.cpp`]);
    await t.cli(["query", "definition", `${id} path:gone.cpp`]);
    t.disk.write("new.cpp", "int fresh() { return 1; }\n");
    await t.cli(["query", "definition", `${id} path:new.cpp`]);
    // By name, a file's own symbol is found where it is declared.
    await t.cli(["query", "symbol-search", '"helper" path:util.h']);
    await t.cli(["query", "symbol-search", '"helper" path:a.cpp']);
});
