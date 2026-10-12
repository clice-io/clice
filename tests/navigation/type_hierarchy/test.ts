import { answers } from "@clice/tools/testing";

// The items a type hierarchy opens on, and their bases and derived types.
answers({ paths: { server: "navigation reads open documents and the index" } }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, {
            type_hierarchy: a.typeHierarchy(m),
            supertypes: a.supertypes(m),
            subtypes: a.subtypes(m),
        });
    }
});
