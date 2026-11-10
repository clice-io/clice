import { answers } from "@clice/tools/testing";

// The marker only makes the server open the header.
answers(async (a) => {
    for (const file of ["main.m", "shape.h"]) {
        await a.show(file, a.documentSymbols(file), { parts: ["detail"] });
        await a.show(`${file} extents`, a.documentSymbols(file), { parts: ["range"] });
    }
});
