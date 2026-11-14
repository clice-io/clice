import { answers, type AnswersContext } from "@clice/tools/testing";

function hints(a: AnswersContext): Promise<void> {
    return a.show("main.cpp", a.inlayHints("main.cpp"));
}

function limited(limit: number | "unlimited"): Record<string, unknown> {
    return { inlay_hints: { default_arguments: true, max_length: { default_arguments: limit } } };
}

// The limit holds the separators and the trailing `…`; "unlimited" lifts it.
answers("unlimited", { config: limited("unlimited") }, hints);
answers("exact fit", { config: limited(19) }, hints);
answers("cut", { config: limited(11) }, hints);
answers("nothing fits", { config: limited(10) }, hints);
answers(
    "no parameter names",
    { config: { inlay_hints: { default_arguments: true, parameters: false } } },
    hints,
);
