import { answers, type AnswersContext } from "@clice/tools/testing";

// The names of system headers rank below the project's.
async function items(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m));
    }
}

answers(items);
