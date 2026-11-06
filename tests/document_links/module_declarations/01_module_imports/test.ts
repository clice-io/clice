import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The module names of the imports are no links, whether the index answers
// before the compile or the AST after it: go-to-definition reaches the
// module.
test("index", async (t) => {
    const s = await t.start({ config: { project: { readonly: "auto" } } });
    s.open("main.cppm", { pull: false });
    await s.sync();
    await t.show("main.cppm", s.documentLinks("main.cppm"));
    expect((await s.counts()).compile).toBe(0);
});

test("compiled", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cppm")).toEqual([]);
    await t.show("main.cppm", {
        links: s.documentLinks("main.cppm"),
        definition: s.definition(t.at("main.cppm", "export import |lib.extra;")),
    });
});
