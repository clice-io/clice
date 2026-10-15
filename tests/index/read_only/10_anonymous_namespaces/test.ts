import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const markers = ["top", "member", "union_member", "nested", "inside"];

// The index's parent chain skips anonymous namespaces, as a qualified name
// does; its cards still name them where the AST cards do.
test(async (t) => {
    const index = await t.start({ config: { project: { readonly: "on" } } });
    index.open("main.cpp", { pull: false });
    await index.sync();
    const ast = await t.start({ config: { project: { readonly: "off" } } });
    ast.open("main.cpp");
    await ast.sync();
    for (const name of markers) {
        const m = t.marker(name);
        await t.show(name, { index: index.hover(m), ast: ast.hover(m) });
    }
    expect((await index.counts()).pch).toBe(0);
});
