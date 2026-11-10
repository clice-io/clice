import { answers } from "@clice/tools/testing";

// The accessors one unit writes, called from another that sees only the
// property: clang declares them there.
answers(
    { paths: { server: "navigation reads open documents and the index" }, indexing: true },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, {
                incoming_calls: a.incomingCalls(m),
                outgoing_calls: a.outgoingCalls(m),
            });
        }
    },
);
