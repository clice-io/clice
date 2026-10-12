import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The tokens a read-only file gets from the index are lexed in the language
// its command forces, not the one its suffix suggests: under C rules
// `class` would be no keyword.
test(async (t) => {
    const s = await t.start({ config: { project: { readonly: "auto" } } });
    s.open("legacy.c", { pull: false });
    await s.sync();
    await t.show("tokens", s.semanticTokens("legacy.c"));
    expect((await s.counts()).pch).toBe(0);
});
