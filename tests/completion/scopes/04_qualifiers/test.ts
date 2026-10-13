import { answers } from "@clice/tools/testing";

// The qualifier leaves out the namespaces the completion point is in, inline
// and anonymous ones, and an unscoped enumeration; a reserved name, or a
// namespace's with one, comes only when the prefix asks for it; a class's
// members never come unqualified.
answers(async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert"] });
    }
});
