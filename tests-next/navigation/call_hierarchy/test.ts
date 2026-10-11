import { answers } from "@clice/tools/testing";

// Navigation reads open documents and the index, which clice inspect has
// neither of. The items a call hierarchy opens on, and their calls both ways.
answers({ paths: { server: "navigation reads open documents and the index" } }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, {
            call_hierarchy: a.callHierarchy(m),
            incoming_calls: a.incomingCalls(m),
            outgoing_calls: a.outgoingCalls(m),
        });
    }
});
