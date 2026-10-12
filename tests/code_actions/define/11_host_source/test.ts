import { answers } from "@clice/tools/testing";

// clice inspect compiles widget.h alone, with no index and no host: it shows
// the definitions it would send to the host source, and Widget::done stays
// missing, since only main.cpp defines it. The server, with the index built,
// places them in main.cpp after the definition already there. Its compile
// check of an edit to main.cpp reads widget.h's diagnostics alone;
// define/18_index_vetted_definitions applies such an edit and reads main.cpp.
answers(
    {
        indexing: true,
        paths: { separate: "the host source and the index exist on the server alone" },
    },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.codeActions(m, { compile: true }));
        }
    },
);
