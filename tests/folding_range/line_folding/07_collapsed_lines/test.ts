import { test } from "@clice/tools/testing";

// Neovim folds whole lines and shows a fold's collapsed text in place of
// its first line.
test("whole lines with collapsed text", async (t) => {
    const s = await t.start({
        capabilities: {
            textDocument: {
                foldingRange: { lineFoldingOnly: true, foldingRange: { collapsedText: true } },
            },
        },
    });
    await s.diagnostics("main.cpp");
    await t.show("main.cpp", s.foldingRanges("main.cpp"));
});
