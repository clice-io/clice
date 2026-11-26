import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

// a.h, answer.h, c.cpp, column.h, main.cpp, own_column.cpp and shapes.h
// start with a UTF-8 byte order mark, as files saved "with BOM" do; data.bin
// starts with the same bytes, and mark.bin is nothing else.

answers("first line", { indexing: true }, async (a) => {
    await a.show("user3", a.hover(a.at("c.cpp", "int |user3")));
    await a.show("tokens", a.semanticTokens("c.cpp"));
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

answers("header with a byte order mark", { indexing: true }, async (a) => {
    await a.show("extent", a.hover(a.marker("extent")));
    await a.show("area", a.hover(a.at("main.cpp", "|area(")));
    await a.show("outline", a.documentSymbols("shapes.h"));
    await a.show("folding", a.foldingRanges("shapes.h"));
    await a.show("tokens", a.semanticTokens("shapes.h"));
});

test("host with a byte order mark keeps the context", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.errors("utils.h")).toEqual([]);
    await t.show("utils.h", s.contexts("utils.h"));
});

test("the compile reads the bytes", async (t) => {
    // #embed reads the mark with the rest of the bytes, whether __has_embed
    // looked the file up or #include read it first, and a file embedded
    // first still includes as source. The compiler counts the mark in a
    // column, of an open document's own first line too, though the editor
    // sends the text without it.
    const s = await t.start();
    for (const file of [
        "embed.cpp",
        "checked.cpp",
        "entered.cpp",
        "embedded_first.cpp",
        "mark_only.cpp",
        "column.cpp",
        "own_column.cpp",
    ]) {
        expect(await s.errors(file)).toEqual([]);
    }
});

test("a save adding the mark", async (t) => {
    // The editor saves the text with the mark the user chose: the document
    // compiles with it from then on, as the compiler would.
    const s = await t.start();
    expect(await s.errors("saved_column.cpp")).toEqual([]);
    t.disk.write("saved_column.cpp", "\uFEFF" + t.disk.read("saved_column.cpp"));
    s.save("saved_column.cpp", { write: false });
    await t.show("after the save", s.errors("saved_column.cpp"));
});

test("a save adding the mark to a header", async (t) => {
    // Alone, the header holds while its first line's column is the one it
    // had before the mark; its host's context holds either way, and the save
    // checks again which one the header compiles in.
    const s = await t.start();
    expect(await s.errors("contexted.h")).toEqual([]);
    t.disk.write("contexted.h", "\uFEFF" + t.disk.read("contexted.h"));
    s.save("contexted.h", { write: false });
    expect(await s.errors("contexted.h")).toEqual([]);
});
