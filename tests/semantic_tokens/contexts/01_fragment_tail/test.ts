import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

// A fragment compiles in its host's context, with what the host includes
// after it appended past its text: none of that is the fragment's.
test("in the editor", async (t) => {
    const s = await t.start();
    expect(await s.errors("colors.def")).toEqual([]);
    await t.show("colors.def", s.semanticTokens("colors.def"));
});

answers(
    "on the command line",
    {
        paths: {
            inspect:
                "the server of an answers test opens sources and marked headers, and colors.def holds no marker",
        },
    },
    (a) => a.show("colors.def", a.semanticTokens("colors.def")),
);
