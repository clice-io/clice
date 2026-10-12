import { answers } from "@clice/tools/testing";

// A project's index is built before its markers are asked. Across files a
// name leads to its definition, its declarations, and its uses in every
// source.
answers(
    { paths: { server: "navigation reads open documents and the index" }, indexing: true },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, {
                definition: a.definition(m),
                declaration: a.declaration(m),
                references: a.references(m),
            });
        }
    },
);
