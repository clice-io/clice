import { test } from "@clice/tools/testing";

// The tokens follow the buffer as typed, before it is saved.
test(async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    await t.show("before", s.semanticTokens(t.marker("name")));
    s.edit("main.cpp", { replace: "int counter = 0;", with: "Gauge counter{};" });
    await s.diagnostics("main.cpp");
    await t.show("type", s.semanticTokens(t.at("main.cpp", "|Gauge counter")));
    await t.show("name", s.semanticTokens(t.at("main.cpp", "Gauge |counter")));
});
