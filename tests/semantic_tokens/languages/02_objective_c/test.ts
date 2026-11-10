import { answers } from "@clice/tools/testing";

// The marker only makes the server open the header.
answers(async (a) => {
    await a.show("main.m", a.semanticTokens("main.m"));
    await a.show("shape.h", a.semanticTokens("shape.h"));
});
