import { test } from "@clice/tools/testing";

// A scope the fragment opens and never closes swallows the rest of its
// includer; the error lands on the brace that opened it, and the scope
// runs to the end of the fragment's own text.
test(async (t) => {
    const s = await t.start();
    await t.show("vec.tpp", s.diagnostics("vec.tpp"));
    await t.show("outline", s.documentSymbols("vec.tpp"), { parts: ["range"] });
});
