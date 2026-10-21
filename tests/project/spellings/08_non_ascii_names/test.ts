import { answers, test } from "@clice/tools/testing";

const MAIN = "源码/主程序.cpp";

answers("names", async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.hover(m));
    }
    await a.show("links", a.documentLinks(MAIN));
});

answers(
    "navigation",
    {
        paths: { server: "clice inspect answers neither definitions nor renames" },
        indexing: true,
    },
    async (a) => {
        await a.show("definition", a.definition(a.marker("call")));
        await a.show("references", a.references(a.marker("call")));
        await a.show("rename", a.rename(a.marker("call"), "assist"));
    },
);

// The commands take the names as arguments, print them and read them back.
test("commands", async (t) => {
    await t.cli("index");
    const outline = await t.cli(["query", "document-symbols", "头文件/工具.h"]);
    const symbols = (outline.json as { result: { symbols: { name: string; symbol: string }[] } })
        .result.symbols;
    const local = symbols.find((each) => each.name === "offset");
    if (local === undefined) {
        throw new Error("the outline of 头文件/工具.h lists no offset");
    }
    await t.cli(["query", "references", local.symbol]);
});
