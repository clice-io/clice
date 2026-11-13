import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

const READONLY = { project: { readonly: "on" } };
const LINES = { textDocument: { foldingRange: { lineFoldingOnly: true } } };

answers("characters", (a) => a.show("main.cpp", a.foldingRanges("main.cpp")));

test("whole lines", async (t) => {
    const s = await t.start({ capabilities: LINES });
    await s.diagnostics("main.cpp");
    await t.show("main.cpp", s.foldingRanges("main.cpp"));
});

// The folds the index serves before the file compiles are the compiled
// file's blocks, placed alike by characters and by whole lines; a block's
// kind the index does not know, the conditionals, and a block whose braces
// the branches of a conditional unbalance wait for the compile.
test("index characters", async (t) => {
    const s = await t.start({ config: READONLY });
    s.open("main.cpp", { pull: false });
    await s.sync();
    await t.show("main.cpp", s.foldingRanges("main.cpp"));
    expect((await s.counts()).compile).toBe(0);
});

test("index whole lines", async (t) => {
    const s = await t.start({ config: READONLY, capabilities: LINES });
    s.open("main.cpp", { pull: false });
    await s.sync();
    await t.show("main.cpp", s.foldingRanges("main.cpp"));
    expect((await s.counts()).compile).toBe(0);
});
