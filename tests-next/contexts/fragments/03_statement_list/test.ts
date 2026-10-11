import { test } from "@clice/tools/testing";

// Statements expanded inside a function body: the context closes the body
// after them.
test(async (t) => {
    const s = await t.start();
    await t.show("handlers.def", s.diagnostics("handlers.def"));
});
