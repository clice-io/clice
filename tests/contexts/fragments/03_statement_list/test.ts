import { test } from "@clice/tools/testing";

// Statements expanded inside a function body: the context closes the body
// after them, and parses it, as the header's own text.
test(async (t) => {
    const s = await t.start();
    await t.show("handlers.def", s.diagnostics("handlers.def"));
    await t.show("alpha", s.hover(t.at("handlers.def", "X(|alpha)")));
});
