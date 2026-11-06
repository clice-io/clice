import { test } from "@clice/tools/testing";

// The module names of the imports are no links, whether the index answers
// before the compile or the AST after it: go-to-definition reaches the
// module.
test("index", async (t) => {
    const s = await t.start({ config: { project: { readonly: "auto" } } });
    s.open("main.cppm", { pull: false });
    await s.sync();
    await t.show("main.cppm", s.documentLinks("main.cppm"));
});

test("compiled", async (t) => {
    const s = await t.start();
    await s.errors("main.cppm");
    await t.show("main.cppm", {
        links: s.documentLinks("main.cppm"),
        definition: s.definition(t.at("main.cppm", "export import |lib.extra;")),
    });
});
