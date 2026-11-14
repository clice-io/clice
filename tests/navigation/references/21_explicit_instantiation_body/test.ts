import { answers } from "@clice/tools/testing";

// Holder<Widget> is instantiated explicitly: reset names Widget's
// constructor at `T(` through the instantiation, once, as an implicit
// instantiation (Holder<Widget>{}.reset()) does and as the stored index
// answers.
answers({ paths: { server: "navigation reads open documents and the index" } }, async (a) => {
    const m = a.marker("constructor");
    await a.show("references", a.references(m));
    await a.show("incoming", a.incomingCalls(m));
});
