import { answers, type AnswersContext } from "@clice/tools/testing";

async function tokens(a: AnswersContext): Promise<void> {
    await a.show("main.cpp", a.semanticTokens("main.cpp"));
}

answers("default", tokens);
answers(
    "renamed",
    {
        config: {
            semantic_tokens: {
                kinds: {
                    field: "property",
                    typeAlias: "type",
                    typeParameter: "type",
                    primitive: "",
                },
            },
        },
    },
    tokens,
);
