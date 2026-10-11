import { answers } from "@clice/tools/testing";

// The standard names come from an included header, as in any real file; the
// server compiles that header into its preamble.
answers(
    {
        paths: {
            separate:
                "the server misses the standard names its preamble declares, which clice inspect finds: the server's blocks are known wrong",
        },
    },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.codeActions(m, { compile: true }));
        }
    },
);
