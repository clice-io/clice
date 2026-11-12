import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The outline the index serves a file before its AST lists, names, nests and
// places every symbol as the compiled outline does, a module's too; the
// details but a macro's parameters wait for the compile.
test("outline", async (t) => {
    const index = await t.start({ config: { project: { readonly: "on" } } });
    index.open("main.cpp", { pull: false });
    index.open("demo.cppm", { pull: false });
    await index.sync();
    const ast = await t.start({ config: { project: { readonly: "off" } } });
    ast.open("main.cpp");
    ast.open("demo.cppm");
    await ast.sync();
    await t.show("index", index.documentSymbols("main.cpp"), { parts: ["range", "detail"] });
    await t.show("ast", ast.documentSymbols("main.cpp"), { parts: ["range"] });
    await t.show("index module", index.documentSymbols("demo.cppm"), {
        parts: ["range", "detail"],
    });
    await t.show("ast module", ast.documentSymbols("demo.cppm"), { parts: ["range"] });
    expect((await index.counts()).compile).toBe(0);
});

// The options leave the same out of the outline the index serves.
test("options", async (t) => {
    const s = await t.start({
        config: { project: { readonly: "on" }, document_symbols: { locals: false, macros: false } },
    });
    s.open("main.cpp", { pull: false });
    await s.sync();
    await t.show("index", s.documentSymbols("main.cpp"));
    expect((await s.counts()).compile).toBe(0);
});
