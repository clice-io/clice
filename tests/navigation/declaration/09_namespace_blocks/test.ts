import { answers } from "@clice/tools/testing";

// Every block of a namespace defines it: declaration answers the block
// definition does, and the first one in the file asking, never every block.
answers(
    { indexing: true, paths: { server: "navigation reads open documents and the index" } },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, { declaration: a.declaration(m), definition: a.definition(m) });
        }
    },
);
