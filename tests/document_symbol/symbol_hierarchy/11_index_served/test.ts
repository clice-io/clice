import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The outline the index serves a file before its AST lists, names, nests and
// places every symbol as the compiled outline does; only the details wait for
// the compile.
test(async (t) => {
    const index = await t.start({ config: { project: { readonly: "on" } } });
    index.open("main.cpp", { pull: false });
    await index.sync();
    const ast = await t.start({ config: { project: { readonly: "off" } } });
    ast.open("main.cpp");
    await ast.sync();
    await t.show("index", index.documentSymbols("main.cpp"), { parts: ["range"] });
    await t.show("ast", ast.documentSymbols("main.cpp"), { parts: ["range"] });
    expect((await index.counts()).compile).toBe(0);
});
