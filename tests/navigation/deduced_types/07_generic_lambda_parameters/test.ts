import { answers } from "@clice/tools/testing";

// Inside the body both questions a user asks about the parameter fail: what
// a member of it is, and what type it has.
answers({ paths: { server: "navigation reads open documents and the index" } }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, { definition: a.definition(m), type_definition: a.typeDefinition(m) });
    }
});
