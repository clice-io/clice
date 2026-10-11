import { test } from "@clice/tools/testing";

// A scope the fragment opens and never closes swallows the rest of its
// includer; the error lands on the brace that opened it.
test(async (t) => {
    const s = await t.start();
    await t.show("vec.tpp", s.diagnostics("vec.tpp"));
});
