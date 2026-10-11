import { answers, type AnswersContext } from "@clice/tools/testing";

function hints(a: AnswersContext): Promise<void> {
    return a.show("main.cpp", a.inlayHints("main.cpp"));
}

answers("default", (a) =>
    a.show("main.cpp", a.inlayHints("main.cpp"), { parts: ["kind", "padding"] }),
);
answers("no parameters", { config: { inlay_hints: { parameters: false } } }, hints);
answers("no deduced types", { config: { inlay_hints: { deduced_types: false } } }, hints);
answers("no designators", { config: { inlay_hints: { designators: false } } }, hints);
answers("disabled", { config: { inlay_hints: { enabled: false } } }, hints);
