import { answers, type AnswersContext } from "@clice/tools/testing";

// A variable holding a lambda is called like a function, unless the context
// expects the lambda itself rather than what a call gives; a function where
// a function pointer is expected is named, not called.
async function items(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert"] });
    }
}

answers(items);
