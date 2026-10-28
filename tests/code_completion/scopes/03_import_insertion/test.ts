import { answers, type AnswersContext } from "@clice/tools/testing";

async function items(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert", "edits"] });
    }
}

const paths = { server: "what the file does not import yet comes from the server's index" };
answers("default", { paths, indexing: true }, items);
answers(
    "configured",
    { paths, indexing: true, config: { code_completion: { insert_imports: false } } },
    items,
);
