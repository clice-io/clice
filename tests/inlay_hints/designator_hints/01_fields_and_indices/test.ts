import { answers, type AnswersContext } from "@clice/tools/testing";

function hints(a: AnswersContext): Promise<void> {
    return a.show("main.cpp", a.inlayHints("main.cpp"));
}

answers("default", hints);
answers("configured", { config: { inlay_hints: { array_indices: true } } }, hints);
answers(
    "indices only",
    { config: { inlay_hints: { designators: false, array_indices: true } } },
    hints,
);
