import { answers } from "@clice/tools/testing";

// A cursor between a name and a separator leading to what it does not spell
// (a range-for's `begin`, a copy's constructor) stands on the name; hover
// and definition agree.
answers({ paths: { server: "navigation reads open documents and the index" } }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, { definition: a.definition(m), hover: a.hover(m) });
    }
});
