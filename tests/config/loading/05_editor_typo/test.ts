import { test } from "@clice/tools/testing";

// The editor's settings misspell a key: a warning, as in clice.toml.
test(async (t) => {
    const s = await t.start({ config: { diagnostics: { clang_tdy: false } } });
    await t.show("guidance", await s.guidance());
});
