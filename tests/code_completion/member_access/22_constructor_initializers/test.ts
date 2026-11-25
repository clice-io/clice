import { answers, type AnswersContext } from "@clice/tools/testing";

async function items(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert", "documentation"] });
    }
}

answers("default", items);
answers("unbundled", { config: { code_completion: { bundle_overloads: false } } }, items);
