import { answers } from "@clice/tools/testing";

answers(
    {
        paths: { server: "what the file does not include yet comes from the server's index" },
        indexing: true,
    },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.completion(m), { parts: ["insert", "edits"] });
        }
    },
);
