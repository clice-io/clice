import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

// Each file's keywords are those of its command's language, standard and GNU
// mode, the compiler's default where it names none: C++'s keywords are
// names in C.
const FILES = ["cxx17.cpp", "gnu.cpp", "default.cpp", "c17.c"];

answers("compiled", async (a) => {
    for (const file of FILES) {
        await a.show(file, a.semanticTokens(file));
    }
});

// The tokens the index serves each file before it compiles lex it alike.
test("index", async (t) => {
    const s = await t.start({ config: { project: { readonly: "on" } } });
    for (const file of FILES) {
        s.open(file, { pull: false });
    }
    await s.sync();
    for (const file of FILES) {
        await t.show(file, s.semanticTokens(file));
    }
    expect((await s.counts()).compile).toBe(0);
});
