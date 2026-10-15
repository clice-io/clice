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
        const card = await index.hover(t.marker(name));
        const compiled = await ast.hover(t.marker(name));
        expect(card?.text.split("\n")[0]).toBe(compiled?.text.split("\n")[0]);
        await t.show(name, card);
    }
    expect((await index.counts()).pch).toBe(0);
});
