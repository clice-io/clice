import { answers } from "@clice/tools/testing";

// A C function a weak and a strong definition give is one symbol with a
// body in each source: the call hierarchy takes each body for an item of
// its own, with the calls written in it.
answers(
    { paths: { server: "navigation reads open documents and the index" }, indexing: true },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, {
                definition: a.definition(m),
                call_hierarchy: a.callHierarchy(m),
                incoming_calls: a.incomingCalls(m),
                outgoing_calls: a.outgoingCalls(m),
            });
        }
    },
);
