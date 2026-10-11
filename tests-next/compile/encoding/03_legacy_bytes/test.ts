import { answers } from "@clice/tools/testing";

// main.cpp is Latin-1, as legacy sources are: its bytes past ASCII are no
// UTF-8. An editor opening it as UTF-8 shows each as a replacement mark.
answers(
    {
        paths: {
            server: "inspect counts the file's raw bytes, which the cases render through the text an editor decodes",
        },
    },
    async (a) => {
        await a.show("after", a.hover(a.at("main.cpp", "int |after")));
        await a.show("tokens", a.semanticTokens("main.cpp"));
    },
);
