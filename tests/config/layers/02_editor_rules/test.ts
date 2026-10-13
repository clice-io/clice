import { answers, test } from "@clice/tools/testing";

const RULES = [
    ["no rules", undefined],
    ["rules from the editor", "**/*.cpp"],
    ["rules that match nothing", "**/does_not_match.cpp"],
] as const;

for (const [name, pattern] of RULES) {
    test(name, async (t) => {
        const s = await t.start(
            pattern === undefined
                ? {}
                : { config: { rules: [{ patterns: [pattern], append: ["-DFROM_EDITOR=1"] }] } },
        );
        await t.show("main.cpp", s.diagnostics("main.cpp"));
    });
}

answers(
    "rules in both clients",
    { config: { rules: [{ patterns: ["**/*.cpp"], append: ["-DFROM_EDITOR=1"] }] } },
    async (a) => {
        await a.show("macro", a.hover(a.marker("macro")));
    },
);
