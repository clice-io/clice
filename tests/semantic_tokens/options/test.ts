import { answers, type AnswersContext } from "@clice/tools/testing";

async function tokens(a: AnswersContext): Promise<void> {
    await a.show("main.cpp", a.semanticTokens("main.cpp"));
}

answers("default", tokens);
answers("semantic only", { config: { semantic_tokens: { lexical: false } } }, tokens);
