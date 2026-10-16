import { test } from "@clice/tools/testing";

// The private module fragment runs to the end of a file without a final
// newline; a client folding whole lines hides its last line too.
test("whole lines", async (t) => {
    t.disk.write("main.cpp", t.disk.read("main.cpp").trimEnd());
    const s = await t.start({
        capabilities: { textDocument: { foldingRange: { lineFoldingOnly: true } } },
    });
    await s.diagnostics("main.cpp");
    await t.show("main.cpp", s.foldingRanges("main.cpp"));
});
