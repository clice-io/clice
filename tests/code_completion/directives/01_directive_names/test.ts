import { answers, type AnswersContext } from "@clice/tools/testing";

async function items(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert"] });
    }
}

// Clang, which answers inspect, still offers `#elif` and `#else` after an `#else`.
const paths = { server: "directive names are answered before anything compiles" };
answers("default", { paths }, items);
answers("configured", { paths, config: { code_completion: { statement_snippets: true } } }, items);
