import { answers, type AnswersContext } from "@clice/tools/testing";

async function hints(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.inlayHints(m));
    }
}

answers("default", hints);
answers("configured", { config: { inlay_hints: { spelled_types: true } } }, hints);
