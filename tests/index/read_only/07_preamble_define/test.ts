import { test } from "@clice/tools/testing";

// A define in the preamble is compiled into the PCH and has no node in the
// file's AST: hover on it falls back to the index, there and only there.
test(async (t) => {
    const s = await t.start({ config: { project: { readonly: "off" } } });
    await s.diagnostics("main.cpp");
    await t.show("define", s.hover(t.marker("limit")));
});
