import { test } from "@clice/tools/testing";

// The editor's settings give a value of the wrong type: none of them applies
// and nothing is served. The test hooks come from clice.toml, which applies
// as it is.
test({ errors: ["initializationOptions: invalid type"] }, async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: "yes" } } });
    s.open("main.cpp");
    await t.show("guidance", await s.guidance());
    await t.show("main.cpp", s.documentSymbols("main.cpp"));
});
