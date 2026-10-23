import { answers, type AnswersContext } from "@clice/tools/testing";

async function items(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert", "edits"] });
    }
}

// The index's macro of a header the file does not include is the same
// label as the local variable: the name the file reaches is the one kept.
answers(
    {
        paths: { server: "what the file does not include yet comes from the server's index" },
        indexing: true,
    },
    items,
);
