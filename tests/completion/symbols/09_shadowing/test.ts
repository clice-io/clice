import { answers } from "@clice/tools/testing";

// A local that shadows a global variable, or hides a same-named function, is
// the one candidate offered by its name: no duplicate entry; the hidden
// function only with the qualifier that still reaches it.
answers(async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m));
    }
});
