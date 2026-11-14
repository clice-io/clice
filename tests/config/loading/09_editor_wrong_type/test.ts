import { test } from "@clice/tools/testing";

// The editor's settings give a value of the wrong type: none of them applies
// and nothing is served.
test(async (t) => {
    const s = await t.start({ config: { poll_seconds: "yes" } });
    s.open("main.cpp");
    await t.show("guidance", await s.guidance());
    await t.show("main.cpp", s.documentSymbols("main.cpp"));
});
