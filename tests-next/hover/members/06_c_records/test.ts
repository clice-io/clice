import { answers } from "@clice/tools/testing";

// C records: a struct's card lists its members, anonymous unions and
// structs included, and tags first declared inside other declarations.
//
// The note is here, not atop the source: a comment there would attach to
// the first declaration as its documentation.
answers(async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.hover(m));
    }
});
