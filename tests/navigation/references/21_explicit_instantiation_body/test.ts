import { answers } from "@clice/tools/testing";

// Holder<Widget> is instantiated explicitly; the same template instantiated
// implicitly (Holder<Widget>{}.reset()) names no constructor in reset, and
// neither does the stored index of either.
answers({ paths: { server: "navigation reads open documents and the index" } }, async (a) => {
    const m = a.marker("constructor");
    await a.show("references", a.references(m));
    await a.show("incoming", a.incomingCalls(m));
});
