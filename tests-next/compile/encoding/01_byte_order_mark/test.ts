import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

// a.h, c.cpp and main.cpp start with a UTF-8 byte order mark, as files
// saved "with BOM" do; data.bin starts with the same bytes.

answers("first line", async (a) => {
    await a.show("user3", a.hover(a.at("c.cpp", "int |user3")));
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
    // The mark is text's, not data's: #embed reads all five bytes.
    const s = await t.start();
    expect(await s.errors("embed.cpp")).toEqual([]);
});
