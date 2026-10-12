import { answers } from "@clice/tools/testing";

// The header lands in the server's preamble, which keeps clang's deferred
// template bodies: the pattern is never instantiated in main.cpp, so the
// server sees no body and synthesizes no accessor documentation. Inspect
// parses the whole unit eagerly and does.
answers(
    {
        paths: {
            separate:
                "the server's preamble keeps template bodies deferred; inspect parses them eagerly",
        },
    },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.hover(m));
        }
    },
);
