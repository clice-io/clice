import { answers } from "@clice/tools/testing";

// A class no @implementation defines, a class of the SDK, is declared by
// its @interface alone: the forward declarations headers repeat lead
// nowhere, for a protocol as for a class.
answers({ paths: { server: "navigation reads open documents and the index" } }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, { definition: a.definition(m), declaration: a.declaration(m) });
    }
});
