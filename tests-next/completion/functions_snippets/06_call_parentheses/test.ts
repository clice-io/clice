import { answers, type AnswersContext } from "@clice/tools/testing";

async function items(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert"] });
    }
}

answers("default", items);
answers(
    "configured",
    { config: { code_completion: { insert_paren_in_function_call: true } } },
    items,
);
