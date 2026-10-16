import { test } from "@clice/tools/testing";

// The preprocessor passes `#` comment lines through to the assembler
// (#791): the editor and the batch index must both get past them.

test("open", async (t) => {
    const s = await t.start();
    await t.show("diagnostics", s.diagnostics("entry.S"));
    await t.show("size", s.hover(t.at("entry.S", ".quad |SIZE")));
});

test("index", async (t) => {
    await t.cli("index --jobs 1");
});
