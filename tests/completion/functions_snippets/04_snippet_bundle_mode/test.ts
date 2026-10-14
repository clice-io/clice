import { answers, type AnswersContext } from "@clice/tools/testing";

// A bundled overload set inserts the parentheses alone: which overload the
// call takes, and so its parameters, is not known yet.
async function items(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert"] });
    }
}

answers("default", items);
answers("none", { config: { code_completion: { arguments: "none" } } }, items);
