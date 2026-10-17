import { test } from "@clice/tools/testing";

// A scope the fragment opens at its includer's top level and never closes
// runs to the end of the fragment's own text, where the error lands, as in
// a file of its own.
test(async (t) => {
    const s = await t.start();
    await t.show("vec.tpp", s.diagnostics("vec.tpp"));
    await t.show("outline", s.documentSymbols("vec.tpp"), { parts: ["range"] });
});
