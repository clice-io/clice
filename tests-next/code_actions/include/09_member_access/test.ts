import { answers } from "@clice/tools/testing";

// The server indexes lib.cpp first, so its index knows the free function
// count from lib.h; inspect answers from the standard library mapping alone.
answers({ indexing: true }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.codeActions(m, { compile: true }));
    }
});
