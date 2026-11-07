import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// shared.h compiles in fast_host.cpp's context, where slow_path() is
// inactive code; slow_host.cpp's context reads it. The answers of the AST
// take it from the index, which merges both.
test("with other contexts", async (t) => {
    const s = await t.start();
    await s.indexed();
    expect(await s.errors("shared.h")).toEqual([]);
    await t.show("shared.h", {
        context: s.currentContext("shared.h"),
        tokens: s.semanticTokens("shared.h"),
        outline: s.documentSymbols("shared.h"),
        folds: s.foldingRanges("shared.h"),
        links: s.documentLinks("shared.h"),
        hover: s.hover(t.marker("slow")),
    });
});

// Asked before the file is open: no answer comes from the index, which
// merges both, while its own compile runs.
test("own context alone", async (t) => {
    const s = await t.start();
    await s.indexed();
    expect((await s.showOtherContexts("shared.h", false)).success).toBe(true);
    s.open("shared.h", { pull: false });
    await t.show("shared.h", {
        context: s.currentContext("shared.h"),
        tokens: s.semanticTokens("shared.h"),
        outline: s.documentSymbols("shared.h"),
        links: s.documentLinks("shared.h"),
        hover: s.hover(t.marker("slow")),
    });
});
