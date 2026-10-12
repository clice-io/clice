import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

test("in the editor", async (t) => {
    const s = await t.start();
    await t.show("diagnostics", s.diagnostics("body.inc"));
    await t.show("symbols", s.documentSymbols("body.inc"));
    const use = t.at("body.inc", "return |field_a");
    await t.show("hover", s.hover(use));
    await t.show("definition", s.definition(use));
    // What comes before the class the fragment sits in is a preamble.
    expect((await s.counts()).pch).toBe(1);
});

answers(
    "on the command line",
    {
        paths: {
            inspect:
                "the server of an answers test opens sources and marked headers, and body.inc holds no marker",
        },
    },
    (a) => a.show("symbols", a.documentSymbols("body.inc")),
);
