import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The card of an include names its file whether the index or the AST
// answers.
test(async (t) => {
    const index = await t.start({ config: { project: { readonly: "on" } } });
    index.open("main.cpp", { pull: false });
    await index.sync();
    const ast = await t.start({ config: { project: { readonly: "off" } } });
    ast.open("main.cpp");
    await ast.sync();
    await t.show("index", index.hover(t.marker("include_path")));
    await t.show("ast", ast.hover(t.marker("include_path")));
    expect((await index.counts()).compile).toBe(0);
});
