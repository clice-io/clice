import { test } from "@clice/tools/testing";

// The fragment leaves its struct open for the includer's brace to close,
// and the includer's namespace stays open to the end of the unit.
test(async (t) => {
    const s = await t.start();
    s.open("x.inc");
    await s.sync();
    await t.show("symbols", s.documentSymbols("x.inc"));
});
