import { answers, type AnswersContext } from "@clice/tools/testing";

async function items(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert"] });
    }
}

// The open main.cpp builds its preamble's PCH over the target's
// precompiled header (base.h), and that PCH's index holds only what
// extra.h adds: a name of base.h comes from the target PCH's index. bare.cpp
// has no preamble of its own and loads the target's PCH alone.
answers(
    { paths: { server: "the names of the build's precompiled header come from its PCH index" } },
    items,
);
