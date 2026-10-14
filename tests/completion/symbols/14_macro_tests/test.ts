import { answers } from "@clice/tools/testing";

// A directive names a macro without invoking it. The directives at the top
// of the file are the ones the server's PCH covers.
answers(async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert"] });
    }
});
