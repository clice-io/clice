import { answers } from "@clice/tools/testing";

// A name spelled in several tokens has one card on each of them:
// destructors, operators, conversion functions, literal operators.
//
// The note is here, not atop the source: a comment there would attach to
// the first declaration as its documentation.
answers(async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.hover(m));
    }
});
