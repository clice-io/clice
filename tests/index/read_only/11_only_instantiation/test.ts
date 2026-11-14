import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const markers = ["member", "abbreviated", "parameter"];

// walk and take are instantiated once, with Widget: the AST resolves the
// `auto` inside them through that instantiation, and the index should
// answer the same.
test(async (t) => {
    const index = await t.start({ lastRule: { readonly: "on" } });
    index.open("main.cpp", { pull: false });
    await index.sync();
    // Beside the first, which keeps the index, a cache of its own.
    const ast = await t.start({ lastRule: { readonly: "off" }, cacheDir: ".clice-ast" });
    ast.open("main.cpp");
    await ast.sync();
    for (const name of markers) {
        const m = t.marker(name);
        await t.show(name, { index: index.definition(m), ast: ast.definition(m) });
    }
    expect((await index.counts()).pch).toBe(0);
});
