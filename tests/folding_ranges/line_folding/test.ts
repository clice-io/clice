import { answers, test } from "@clice/tools/testing";

answers("characters", (a) => a.show("main.cpp", a.foldingRanges("main.cpp")));

// A lineFoldingOnly client hides whole lines below the start line: a fold
// ends before the line holding its closing brace or the next section's
// header, and a block whose brace sits below its name starts on the name.
test("whole lines", async (t) => {
    const s = await t.start({
        capabilities: { textDocument: { foldingRange: { lineFoldingOnly: true } } },
    });
    await s.diagnostics("main.cpp");
    await t.show("main.cpp", s.foldingRanges("main.cpp"));
});
