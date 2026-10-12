import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start();
    await t.show("diagnostics", s.diagnostics("errors.def"));
    await t.show("symbols", s.documentSymbols("errors.def"));
    await t.show("hover", s.hover(t.at("errors.def", "ERROR(|NotFound")));
});
