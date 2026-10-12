import { answers } from "@clice/tools/testing";

// Every line ends in CRLF, as files checked out on Windows do.
answers(async (a) => {
    await a.show("helper", a.hover(a.at("main.cpp", "return |helper")));
    await a.show("links", a.documentLinks("main.cpp"));
    await a.show("tokens", a.semanticTokens("main.cpp"));
});
