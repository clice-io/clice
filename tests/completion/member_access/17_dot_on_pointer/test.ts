import { answers, type AnswersContext } from "@clice/tools/testing";

async function items(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert", "edits"] });
    }
}

answers("default", items);
answers("configured", { config: { code_completion: { dot_to_arrow: false } } }, items);
