import { answers } from "@clice/tools/testing";

// The macros subtopic's requests, and the subtypes the base class lists:
// the pasted class is one of them.
answers({ paths: { server: "navigation reads open documents and the index" } }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, {
            definition: a.definition(m),
            references: a.references(m),
            subtypes: a.subtypes(m),
        });
    }
});
