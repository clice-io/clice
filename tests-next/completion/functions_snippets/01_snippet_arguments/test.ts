import { answers, type AnswersContext } from "@clice/tools/testing";

async function snippets(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert"] });
    }
}

answers("default", snippets);
answers(
    "configured",
    {
        config: {
            code_completion: { bundle_overloads: false, enable_function_arguments_snippet: true },
        },
    },
    snippets,
);
