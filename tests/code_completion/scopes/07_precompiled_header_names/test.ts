import { answers, type AnswersContext } from "@clice/tools/testing";

async function items(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert"] });
    }
}

// The open main.cpp builds its preamble's PCH over the target's
// precompiled header (base.h), which holds only what extra.h adds; bare.cpp
// has no preamble of its own and loads the target's PCH alone. The macros
// of base.h come from the target's PCH the parse loads, the names of other
// namespaces from the index: base.h's from the build of the target's PCH,
// which indexes the unit writing it, extra.h's from main.cpp's.
answers(
    {
        paths: { server: "the build's precompiled header is the server's" },
        indexing: true,
    },
    items,
);
