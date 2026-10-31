import { answers } from "@clice/tools/testing";

// The names of lib.h come to the server's compile from the preamble's
// index, which loads just them: through a using-declaration, a
// using-directive, a namespace's own name and an alias of another name; of
// internal linkage too.
answers(async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert"] });
    }
});
