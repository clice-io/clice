import { answers, type AnswersContext } from "@clice/tools/testing";

async function snippets(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert"] });
    }
}

answers("default", snippets);
answers("delimiters", { config: { code_completion: { arguments: "delimiters" } } }, snippets);
answers("none", { config: { code_completion: { arguments: "none" } } }, snippets);
