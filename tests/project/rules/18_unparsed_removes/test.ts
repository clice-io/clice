import { test } from "@clice/tools/testing";

// Each rule names an option without its value, or -W with a wildcard, and
// takes every such flag out.
test(async (t) => {
    const s = await t.start();
    await t.show("a.cpp", s.diagnostics("a.cpp"));
    await t.show("b.cpp", s.diagnostics("b.cpp"));
    await t.show("c.cpp", s.diagnostics("c.cpp"));
});
