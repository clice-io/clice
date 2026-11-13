import { answers } from "@clice/tools/testing";

// No source includes widget.h. Out of line in the header a definition is
// marked inline, but for what other files cannot see. clice inspect also
// shows what a host source would receive; the server, which has no host to
// send it to, does not offer it.
answers(
    { paths: { separate: "only clice inspect shows the definitions meant for a host" } },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.codeActions(m, { compile: true }));
        }
    },
);
