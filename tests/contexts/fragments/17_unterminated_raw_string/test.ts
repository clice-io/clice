import { test } from "@clice/tools/testing";

// m.inc ends inside a raw string literal, which runs on into the includer's
// code the context appends after it: the error ends with m.inc's text.
test(async (t) => {
    const s = await t.start();
    await t.show("m.inc", s.diagnostics("m.inc"));
});
