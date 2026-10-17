import { answers } from "@clice/tools/testing";

// The fallback looks the declaration up by name, in the project index the
// background indexer keeps.
answers(
    { indexing: true, paths: { server: "the fallback searches the project index" } },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.declaration(m));
        }
    },
);
