import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

// a.h, c.cpp, first.cpp and main.cpp start with a UTF-8 byte order mark, as
// files saved "with BOM" do; data.bin starts with the same bytes.

answers("first line", async (a) => {
    await a.show("user3", a.hover(a.at("c.cpp", "int |user3")));
    await a.show("folds", a.foldingRanges("first.cpp"));
    await a.show("auto", a.codeActions(a.at("first.cpp", "|auto answer"), { compile: true }));
    await a.show("completion", a.completion(a.at("first.cpp", "aut|o answer")), {
        parts: ["edit"],
    });
});

answers("formatting", { paths: { server: "clice inspect does not format" } }, async (a) => {
    await a.show("first.cpp", a.formatting("first.cpp"));
});

answers(
    "navigation",
    { indexing: true, paths: { server: "navigation reads the index" } },
    async (a) => {
        await a.show("shared_fn", a.definition(a.at("main.cpp", "|shared_fn()")));
        await a.show("user3", a.workspaceSymbols("user3"));
    },
);

test("marked host keeps the context", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.errors("utils.h")).toEqual([]);
    await t.show("utils.h", s.contexts("utils.h"));
});

test("embedded data keeps the mark", async (t) => {
    // The mark is text's, not data's: #embed reads all five bytes, after
    // __has_embed looked the file up too.
    const s = await t.start();
    expect(await s.errors("embed.cpp")).toEqual([]);
    expect(await s.errors("checked.cpp")).toEqual([]);
});

// The open buffer holds the mark of the file on disk, which the editor's text
// leaves out: the file's index serves the unedited buffer, and goes on
// serving it once a save drops the mark or adds it back.
test("saves move the mark", async (t) => {
    const s = await t.start({ config: { project: { readonly: "on" } } });
    s.open("c.cpp", { pull: false });
    await s.sync();
    await t.show("marked", s.documentSymbols("c.cpp"));
    s.save("c.cpp");
    await s.sync();
    await t.show("unmarked", s.documentSymbols("c.cpp"));
    t.disk.write("c.cpp", `\ufeff${t.disk.read("c.cpp")}`);
    s.save("c.cpp", { write: false });
    await s.sync();
    await t.show("marked again", s.documentSymbols("c.cpp"));
});
