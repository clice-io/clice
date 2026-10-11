import { test } from "@clice/tools/testing";

// The usual session: one file open, the rest of the project known only
// through the background index.
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    s.open("main.cpp");
    const m = t.marker("use");
    await t.show("use", {
        definition: s.definition(m),
        declaration: s.declaration(m),
        references: s.references(m),
        incoming_calls: s.incomingCalls(m),
    });
});
