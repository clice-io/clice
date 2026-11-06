import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A global module fragment's own declarations go into the preamble with its
// directives: left to the body, they would put the module declaration after
// a declaration the body parsed itself. Read back from the PCH, they show in
// the file's outline, a namespace's members under it, and in its tokens,
// folds and hovers; the fragment's disabled region stays text. impl.cppm and
// other.cppm open with the same fragment and build a PCH each, which places
// the declarations in the file itself; counter.cppm's fragment imports and
// goes without one. The open file's index has them too.
test("compiles", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    for (const file of ["impl.cppm", "other.cppm", "lib.cppm", "counter.cppm", "main.cpp"]) {
        expect(await s.errors(file)).toEqual([]);
    }
    await s.sync();
    const { files } = await s.counts();
    expect([files["impl.cppm"]?.pch, files["other.cppm"]?.pch, files["counter.cppm"]?.pch]).toEqual(
        [1, 1, 0],
    );
    await t.show("outline", s.documentSymbols("other.cppm"));
    await t.show("tokens", s.semanticTokens("other.cppm"));
    await t.show("folding", s.foldingRanges("other.cppm"));
    await t.show("definition", s.definition(t.marker("use")));
    await t.show("references", s.references(t.marker("use")));
    await t.show("hover", s.hover(t.marker("fwd")));
    await t.show("reference hover", s.hover(t.marker("width")));
    await t.show("symbols", s.workspaceSymbols("Forward"));
});

// An edit in the fragment builds its PCH again: an error in a definition
// the fragment writes shows where it is, and an #include still being typed
// above the fragment's declarations leaves the file to parse whole, with
// the include's own error.
test("edits", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    s.open("other.cppm");
    s.edit("other.cppm", { replace: "    return 1;", with: "    return missing;" });
    await t.show("error", s.errors("other.cppm"));
    s.edit("other.cppm", { replace: "    return missing;", with: "    return 1;" });
    s.edit("other.cppm", {
        replace: '#include "legacy.h"',
        with: '#include "legacy.h"\n#include "leg',
    });
    await t.show("typing", s.errors("other.cppm"));
});
