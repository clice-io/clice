import { test } from "@clice/tools/testing";

// A brace the user typed and left open takes the closer the context adds:
// the errors show, and the server keeps answering.
test(async (t) => {
    const s = await t.start();
    await t.show("diagnostics", s.diagnostics("list.def"));
    await t.show("contexts", s.contexts("list.def"));
});
