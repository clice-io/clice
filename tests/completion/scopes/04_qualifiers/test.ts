import { answers } from "@clice/tools/testing";

// The qualifier leaves out the namespaces the completion point is in, inline
// and anonymous ones, and an unscoped enumeration, but starts from the global
// scope where a closer scope takes its first name. A reserved name comes only
// when the prefix asks for it, a reserved namespace's never; a class's members
// never come unqualified, nor a template's specializations beside it; where
// only a type may be written, only what names one.
answers(async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert"] });
    }
});
