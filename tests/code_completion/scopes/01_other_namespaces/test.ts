import { answers, type AnswersContext } from "@clice/tools/testing";

async function items(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert"] });
    }
}

// The names of other namespaces come from the index.
answers("default", { indexing: true }, items);
answers("configured", { config: { code_completion: { all_scopes: false } } }, items);
