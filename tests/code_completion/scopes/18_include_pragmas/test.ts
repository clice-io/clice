import { answers, type AnswersContext } from "@clice/tools/testing";

async function items(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert", "edits"] });
    }
}

answers(
    {
        paths: { server: "what the file does not include yet comes from the server's index" },
        indexing: true,
    },
    items,
);
