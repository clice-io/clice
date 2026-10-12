import { answers } from "@clice/tools/testing";

// A subtopic about what a name leads to asks go to definition.
answers({ paths: { server: "navigation reads open documents and the index" } }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.definition(m));
    }
});
