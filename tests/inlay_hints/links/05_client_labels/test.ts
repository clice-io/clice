import { test } from "@clice/tools/testing";

// A client without LSP 3.17 inlay hint support gets each label as one plain
// string; one with it gets the pieces, a named type linking to its
// declaration.
test("plain client", async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    await t.show("main.cpp", s.inlayHints("main.cpp"), { parts: ["links"] });
});

test("inlay hint client", async (t) => {
    const s = await t.start({ capabilities: { textDocument: { inlayHint: {} } } });
    s.open("main.cpp");
    await t.show("main.cpp", s.inlayHints("main.cpp"), { parts: ["links"] });
});
