import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Both files open with the same leading directives and share one precompiled
// preamble; each sees the warnings of its build pointing into itself, and the
// command line's warning once.
test(async (t) => {
    const s = await t.start();
    for (const file of ["a.cpp", "b.cpp"]) {
        await t.show(file, s.diagnostics(file), { parts: ["related"] });
    }
    expect((await s.counts()).pch).toBe(1);
});
