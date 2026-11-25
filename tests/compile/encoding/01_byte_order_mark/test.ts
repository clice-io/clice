import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

// a.h, answer.h, c.cpp, column.h, main.cpp and shapes.h start with a UTF-8
// byte order mark, as files saved "with BOM" do; data.bin starts with the
// same bytes, and mark.bin is nothing else.

answers("first line", async (a) => {
    await a.show("user3", a.hover(a.at("c.cpp", "int |user3")));
});

answers(
    "navigation",
    { indexing: true, paths: { server: "navigation reads the index" } },
    async (a) => {
        await a.show("shared_fn", a.definition(a.at("main.cpp", "|shared_fn()")));
        await a.show("user3", a.workspaceSymbols("user3"));
        await a.show("ANSWER", a.definition(a.at("embedded_first.cpp", "|ANSWER ==")));
    },
);

answers("marked header", { indexing: true }, async (a) => {
    await a.show("extent", a.hover(a.marker("extent")));
    await a.show("area", a.hover(a.at("main.cpp", "|area(")));
    await a.show("outline", a.documentSymbols("shapes.h"));
    await a.show("folding", a.foldingRanges("shapes.h"));
    await a.show("tokens", a.semanticTokens("shapes.h"));
});

test("marked host keeps the context", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.errors("utils.h")).toEqual([]);
    await t.show("utils.h", s.contexts("utils.h"));
});

test("the compile reads the bytes", async (t) => {
    // The mark is text's, not data's: #embed reads it with the rest, whether
    // __has_embed looked the file up or #include read it first, and a file
    // embedded first still includes as its text. The compiler counts it in
    // a column.
    const s = await t.start();
    for (const file of [
        "embed.cpp",
        "checked.cpp",
        "entered.cpp",
        "embedded_first.cpp",
        "mark_only.cpp",
        "column.cpp",
    ]) {
        expect(await s.errors(file)).toEqual([]);
    }
});
