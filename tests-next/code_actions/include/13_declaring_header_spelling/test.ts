import { answers } from "@clice/tools/testing";

// The directive spells the path that finds the header declaring the name:
// "util.h" would find first/util.h through -Ifirst, so the action writes
// "second/util.h".

answers(
    {
        indexing: true,
        paths: {
            server: "the declaring header comes from the project index, which only the server builds",
        },
    },
    async (a) => {
        await a.show("shadowed", a.codeActions(a.marker("shadowed"), { compile: true }));
    },
);
