import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The compile crashes where clang instantiates, past the parse, every
// time. The PCH it read is the first suspect: rebuilt, it crashes the
// compile again, which then stands stopped.
test(async (t) => {
    const s = await t.start({
        config: { diagnostics: { clang_tidy: false }, project: { enable_indexing: false } },
        crash: { request: "finishCompile", file: "main.cpp" },
    });
    await t.show("main.cpp", s.diagnostics("main.cpp"));
    await t.show("crashes", await s.crashes());
    const { files } = await s.counts();
    expect(files["main.cpp"]?.pch).toBe(2);
});
