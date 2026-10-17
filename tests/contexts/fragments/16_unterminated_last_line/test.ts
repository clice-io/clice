import { test } from "@clice/tools/testing";

// m.inc's last line, the end of its text, has no line break: the error at
// its end belongs to the text, not to the suffix the context appends.
test(async (t) => {
    const s = await t.start();
    await t.show("m.inc", s.diagnostics("m.inc"));
});
