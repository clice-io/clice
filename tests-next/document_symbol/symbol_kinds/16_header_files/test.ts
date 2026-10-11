import { answers } from "@clice/tools/testing";

// The outline of a header the editor opens, in the context of the source
// that includes it. The marker only makes the server open the header.
answers(async (a) => {
    await a.show("outline", a.documentSymbols("widget.h"), { parts: ["detail"] });
    await a.show("extents", a.documentSymbols("widget.h"), { parts: ["range"] });
});
