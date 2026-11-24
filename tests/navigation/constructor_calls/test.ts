import { answers, test } from "@clice/tools/testing";

// A class's name that calls a constructor stands for it in every feature but
// the type ones, which answer the class.
answers("cards", async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, {
            hover: a.hover(m),
            highlights: a.highlights(m),
            semantic_token: a.semanticTokens(m),
        });
    }
});

answers(
    "navigation",
    { paths: { server: "navigation reads open documents and the index" } },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, {
                definition: a.definition(m),
                declaration: a.declaration(m),
                type_definition: a.typeDefinition(m),
                references: a.references(m),
            });
        }
    },
);

// Read-only, the card comes from the index.
test("index cards", async (t) => {
    const s = await t.start({ lastRule: { readonly: "on" } });
    for (const file of new Set(t.markers().map((m) => m.path))) {
        s.open(file, { pull: false });
    }
    await s.sync();
    for (const m of t.markers()) {
        await t.show(m.name, s.hover(m));
    }
});
