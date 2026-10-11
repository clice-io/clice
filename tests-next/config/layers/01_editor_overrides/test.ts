import { answers, test } from "@clice/tools/testing";

answers("rules from the file", async (a) => {
    await a.show("symbols", a.documentSymbols("main.cpp"));
    await a.show("hover", a.hover(a.at("main.cpp", "int |main")));
});

test("rules from the editor", async (t) => {
    const s = await t.start({
        config: { rules: [{ patterns: ["**/*.cpp"], append: ["-DUNRELATED"] }] },
    });
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
