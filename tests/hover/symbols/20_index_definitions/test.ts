import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const files = ["main.cpp", "math.cppm", "shapes.m"];

// Read-only, a definition's card comes from the index: the declaration cut
// from the stored text as the AST's card prints it, but a type's members,
// which need the AST.
test(async (t) => {
    const index = await t.start({ config: { project: { readonly: "on" } } });
    for (const file of files) {
        index.open(file, { pull: false });
    }
    await index.sync();
    const ast = await t.start({ config: { project: { readonly: "off" } } });
    for (const file of files) {
        ast.open(file);
    }
    await ast.sync();
    for (const m of t.markers()) {
        await t.show(m.name, { index: index.hover(m), ast: ast.hover(m) });
    }
    expect((await index.counts()).pch).toBe(0);
});
