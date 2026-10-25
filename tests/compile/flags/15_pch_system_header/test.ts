import { test } from "@clice/tools/testing";

// CMake's cmake_pch.hxx opens with `#pragma clang system_header`; the
// headers it includes stay the project's.

test("system header", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await s.diagnostics("main.cpp");
    await t.show("tokens", s.semanticTokens("main.cpp"));
});
