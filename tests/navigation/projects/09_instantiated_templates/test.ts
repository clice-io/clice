import { test } from "@clice/tools/testing";

// Only the closed source instantiates `measure` with `Box` and expands
// `RESET_ALL`: the index carries what both resolve in the headers.
test("closed source", async (t) => {
    const s = await t.start();
    await s.indexed();
    s.open("main.cpp");
    await s.sync();
    for (const name of ["open", "reset"]) {
        const m = t.marker(name);
        await t.show(name, {
            references: s.references(m),
            incoming_calls: s.incomingCalls(m),
        });
    }
    await t.show("measure", { outgoing_calls: s.outgoingCalls(t.marker("measure")) });
});

// An open header's own compile instantiates nothing: what the closed
// source resolves there still answers while the buffer is the indexed text.
test("open header", async (t) => {
    const s = await t.start();
    await s.indexed();
    s.open("main.cpp");
    s.open("measure.h");
    s.open("box.h");
    await s.sync();
    for (const name of ["open", "reset"]) {
        await t.show(name, { references: s.references(t.marker(name)) });
    }
});
