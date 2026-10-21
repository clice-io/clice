import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const markers = ["member", "abbreviated", "parameter"];

// walk and take are instantiated once, with Widget: the AST resolves the
// `auto` inside them through that instantiation, and the index should
// answer the same.
test(async (t) => {
    const index = await t.start({ config: { project: { readonly: "on" } } });
    index.open("main.cpp", { pull: false });
    await index.sync();
    const ast = await t.start({ config: { project: { readonly: "off" } } });
    ast.open("main.cpp");
    await ast.sync();
    for (const name of markers) {
        const m = t.marker(name);
        await t.show(name, { index: index.definition(m), ast: ast.definition(m) });
    }
    expect((await index.counts()).pch).toBe(0);
});
