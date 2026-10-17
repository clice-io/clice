import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Both files open with the same leading directives and share one precompiled
// preamble, whose warning has a note in the file that built it: each file
// sees the note in itself.
test(async (t) => {
    const s = await t.start();
    for (const file of ["a.cpp", "b.cpp"]) {
        await t.show(file, s.diagnostics(file), { parts: ["related"] });
    }
    expect((await s.counts()).pch).toBe(1);
});
