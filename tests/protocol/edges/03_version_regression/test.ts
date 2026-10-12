import { test } from "@clice/tools/testing";

// A version that goes backwards is the editor's bug: the change is applied
// all the same.

test(async (t) => {
    const s = await t.start();
    s.open("main.cpp", { version: 5 });
    await s.diagnostics("main.cpp");
    // A version below the open's, which the actions never send.
    s.client.change(s.uri("main.cpp"), 3, `${t.disk.read("main.cpp")}int bad(\n`);
    await t.show("hover", s.hover(t.marker("add")));
    const pulled = await s.client.pullDiagnostics(s.uri("main.cpp"));
    // The positions are in the changed buffer the test does not hold.
    await t.show(
        "diagnostics",
        pulled.map(({ message }) => (typeof message === "string" ? message : message.value)),
    );
});
