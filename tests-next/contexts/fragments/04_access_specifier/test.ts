import { test } from "@clice/tools/testing";

// A fragment that opens with an access specifier, under a .tcc name.
test(async (t) => {
    const s = await t.start();
    await t.show("body.tcc", s.diagnostics("body.tcc"));
});
