import { test } from "@clice/tools/testing";

// The editor's settings misspell a key that clice.toml would warn about.
test(async (t) => {
    const s = await t.start({ config: { diagnostics: { clang_tdy: false } } });
    await t.show("guidance", await s.guidance());
});
