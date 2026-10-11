import { expect } from "vitest";
import { generate, test } from "@clice/tools/testing";

// Past 8 MiB the server stops lexing a file whole: semantic tokens and
// folding ranges are not served, while what the index rows answer still
// is.
const MAIN = `#include "header.h"

/// Doubles a value.
int twice(int x) {
    return add(x, x);
}

int main() { return twice(2); }
`;

test(async (t) => {
    t.disk.write("main.cpp", generate.padded(MAIN, 9 * 1024 * 1024));
    const s = await t.start({ config: { project: { readonly: "auto" } } });
    s.open("main.cpp", { pull: false });
    await s.sync();
    await t.show("outline", s.documentSymbols("main.cpp"));
    await t.show("links", s.documentLinks("main.cpp"));
    await t.show("hover", s.hover(t.at("main.cpp", "return |add(x, x)")));
    await t.show("tokens", s.semanticTokens("main.cpp"));
    await t.show("folds", s.foldingRanges("main.cpp"));
    expect((await s.counts()).pch).toBe(0);
});
