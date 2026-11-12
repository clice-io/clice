import { answers } from "@clice/tools/testing";

// Shared declarations rank above the same name, the two together above
// either. The first is taken directly only when it beats every other on
// each kind of evidence: src/tables.cpp defines more of codec.h than
// src/codec.cpp, and src/parser.cpp, defining nothing parser.h declares,
// still has the name src/lexer.cpp lacks.
answers(
    { paths: { server: "counterparts read the build and the project index" }, indexing: true },
    async (a) => {
        for (const file of ["include/codec.h", "include/parser.h"]) {
            await a.show(file, a.counterparts(file));
        }
    },
);
