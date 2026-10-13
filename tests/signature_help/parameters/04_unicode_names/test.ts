import { answers } from "@clice/tools/testing";

// A parameter named in characters past ASCII, one of them past the BMP: the
// label's parameter offsets count UTF-16 units, so the active parameter's
// brackets land on it.
answers(async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.signatureHelp(m));
    }
});
