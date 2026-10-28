import { answers } from "@clice/tools/testing";

// The #define and the #include expanding it are leading directives: the
// server's preamble runs them, and what a preamble expands answers from
// the background index. inspect builds no preamble.
answers(
    "open",
    {
        paths: {
            separate:
                "the server resolves what its preamble expands from the background index; inspect builds no preamble",
        },
    },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.highlights(m));
        }
    },
);

answers("indexed", { indexing: true }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.highlights(m));
    }
});
